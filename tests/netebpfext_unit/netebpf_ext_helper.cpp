// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT

#include "net_ebpf_ext_sock_addr.h"
#include "netebpf_ext_helper.h"

DEVICE_OBJECT* _net_ebpf_ext_driver_device_object;

constexpr uint32_t _test_destination_ipv4_address = 0x01020304;
static FWP_BYTE_ARRAY16 _test_destination_ipv6_address = {1, 2, 3, 4};
constexpr uint16_t _test_destination_port = 1234;
constexpr uint32_t _test_source_ipv4_address = 0x05060708;
static FWP_BYTE_ARRAY16 _test_source_ipv6_address = {5, 6, 7, 8};
constexpr uint16_t _test_source_port = 5678;
constexpr uint8_t _test_protocol = IPPROTO_TCP;
constexpr uint32_t _test_compartment_id = 1;
static FWP_BYTE_BLOB _test_app_id = {.size = 2, .data = (uint8_t*)"\\"};
static uint64_t _test_interface_luid = 1;
static TOKEN_ACCESS_INFORMATION _test_token_access_information = {0};
static FWP_BYTE_BLOB _test_user_id = {
    .size = (sizeof(TOKEN_ACCESS_INFORMATION)), .data = (uint8_t*)&_test_token_access_information};

void
netebpfext_initialize_fwp_classify_parameters(_Out_ fwp_classify_parameters_t* parameters)
{
    parameters->destination_ipv4_address = _test_destination_ipv4_address;
    parameters->destination_ipv6_address = _test_destination_ipv6_address;
    parameters->source_ipv4_address = _test_source_ipv4_address;
    parameters->source_ipv6_address = _test_source_ipv6_address;
    parameters->source_port = _test_source_port;
    parameters->destination_port = _test_destination_port;
    parameters->protocol = _test_protocol;
    parameters->compartment_id = _test_compartment_id;
    parameters->app_id = _test_app_id;
    parameters->interface_luid = _test_interface_luid;
    parameters->token_access_information = _test_token_access_information;
    parameters->user_id = _test_user_id;
}

_netebpf_ext_helper::_netebpf_ext_helper(bool initialize_platform)
    : _netebpf_ext_helper(nullptr, nullptr, nullptr, initialize_platform)
{}

_netebpf_ext_helper::_netebpf_ext_helper(
    _In_opt_ const void* npi_specific_characteristics,
    _In_opt_ _ebpf_extension_dispatch_function dispatch_function,
    _In_opt_ netebpfext_helper_base_client_context_t* client_context,
    bool initialize_platform)
{
    // Do not use REQUIRE() in this constructor or the destructor will never be called
    // to clean up any state allocated before the REQUIRE.

    if (!NT_SUCCESS(net_ebpf_ext_trace_initiate())) {
        return;
    }
    trace_initiated = true;

    if (initialize_platform) {
        if (ebpf_platform_initiate() != EBPF_SUCCESS) {
            return;
        }
        platform_initialized = true;
    }

    if (!NT_SUCCESS(net_ebpf_ext_initialize_ndis_handles(driver_object))) {
        return;
    }

    ndis_handle_initialized = true;

    if (!NT_SUCCESS(net_ebpf_extension_initialize_wfp_components(device_object))) {
        return;
    }

    wfp_initialized = true;

    if (!NT_SUCCESS(net_ebpf_ext_register_providers())) {
        return;
    }

    provider_registered = true;

    nmr_program_info_client_handle = std::make_unique<nmr_client_registration_t>(&program_info_client, this);

    this->hook_invoke_function = dispatch_function;
    if (dispatch_function != nullptr && client_context != nullptr) {
        hook_client.ClientRegistrationInstance.NpiSpecificCharacteristics = npi_specific_characteristics;
        client_context->helper = this;
        nmr_hook_client_handle = std::make_unique<nmr_client_registration_t>(&hook_client, client_context);
    }

    usersim_fwp_set_sublayer_guids(
        EBPF_DEFAULT_SUBLAYER, EBPF_HOOK_CGROUP_CONNECT_V4_SUBLAYER, EBPF_HOOK_CGROUP_CONNECT_V6_SUBLAYER);
}

_netebpf_ext_helper::~_netebpf_ext_helper()
{
    if (nmr_hook_client_handle) {
        nmr_hook_client_handle.reset(nullptr);
    }

    if (nmr_program_info_client_handle) {
        nmr_program_info_client_handle.reset(nullptr);
    }

    if (provider_registered) {
        net_ebpf_ext_unregister_providers();
    }

    if (wfp_initialized) {
        net_ebpf_extension_uninitialize_wfp_components();
    }

    if (ndis_handle_initialized) {
        net_ebpf_ext_uninitialize_ndis_handles();
    }

    if (platform_initialized) {
        ebpf_platform_terminate();
    }

    if (trace_initiated) {
        net_ebpf_ext_trace_terminate();
    }
}

std::vector<GUID>
_netebpf_ext_helper::program_info_provider_guids()
{
    std::vector<GUID> guids;
    for (const auto& [id, provider] : program_info_providers) {
        guids.push_back(id);
    }
    return guids;
}

ebpf_program_data_t*
_netebpf_ext_helper::get_program_info_provider_data(_In_ const GUID& program_info_provider)
{
    auto iter = program_info_providers.find(program_info_provider);

    // We might not find the provider if some allocation failed during initialization.
    if (iter != program_info_providers.end()) {
        return const_cast<ebpf_program_data_t*>(iter->second->program_data);
    }

    return nullptr;
}

NTSTATUS
_netebpf_ext_helper::_program_info_client_attach_provider(
    _In_ HANDLE nmr_binding_handle,
    _Inout_ void* client_context,
    _In_ const NPI_REGISTRATION_INSTANCE* provider_registration_instance)
{
    auto& helper = *reinterpret_cast<_netebpf_ext_helper*>(client_context);
    auto client_binding_context = std::make_unique<program_info_provider_t>();
    client_binding_context->module_id = *provider_registration_instance->ModuleId;
    client_binding_context->parent = &helper;
    client_binding_context->program_data =
        reinterpret_cast<const ebpf_program_data_t*>(provider_registration_instance->NpiSpecificCharacteristics);

    NTSTATUS status = NmrClientAttachProvider(
        nmr_binding_handle,
        client_binding_context.get(),
        &client_binding_context,
        &client_binding_context->context,
        &client_binding_context->dispatch);

    if (NT_SUCCESS(status)) {
        helper.program_info_providers[provider_registration_instance->ModuleId->Guid].reset(
            client_binding_context.release());
    }
    return status;
}

NTSTATUS
_netebpf_ext_helper::_program_info_client_detach_provider(_Inout_ void* client_binding_context)
{
    UNREFERENCED_PARAMETER(client_binding_context);
    return STATUS_SUCCESS;
}

void
_netebpf_ext_helper::_program_info_client_cleanup_binding_context(_In_ _Post_invalid_ void* client_binding_context)
{
    UNREFERENCED_PARAMETER(client_binding_context);
}

NTSTATUS
_netebpf_ext_helper::_hook_client_attach_provider(
    _In_ HANDLE nmr_binding_handle,
    _Inout_ void* client_context,
    _In_ const NPI_REGISTRATION_INSTANCE* provider_registration_instance)
{
    UNREFERENCED_PARAMETER(provider_registration_instance);
    const void* provider_dispatch_table;
    auto base_client_context = reinterpret_cast<netebpfext_helper_base_client_context_t*>(client_context);
    if (base_client_context == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const ebpf_extension_dispatch_table_t client_dispatch_table = {
        .version = 1, .count = 1, .function = base_client_context->helper->hook_invoke_function};
    auto provider_data = (const ebpf_attach_provider_data_t*)provider_registration_instance->NpiSpecificCharacteristics;
    if (base_client_context->desired_attach_type != BPF_ATTACH_TYPE_UNSPEC &&
        provider_data->bpf_attach_type != base_client_context->desired_attach_type) {
        return STATUS_ACCESS_DENIED;
    }

    return NmrClientAttachProvider(
        nmr_binding_handle,
        client_context, // Client binding context.
        &client_dispatch_table,
        &base_client_context->provider_binding_context,
        &provider_dispatch_table);
}

NTSTATUS
_netebpf_ext_helper::_hook_client_detach_provider(_Inout_ void* client_binding_context)
{
    UNREFERENCED_PARAMETER(client_binding_context);

    // All callbacks we implement are done.
    return STATUS_SUCCESS;
}

void
_netebpf_ext_helper::_hook_client_cleanup_binding_context(_In_ void* client_binding_context)
{
    UNREFERENCED_PARAMETER(client_binding_context);
}
