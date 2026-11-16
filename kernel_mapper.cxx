#include <impl/include.hxx>

#include <cstring>
#include <filesystem>
#include <sstream>

std::wstring get_exe_directory(void);

int wmain(int argc, wchar_t* argv[]) {
    std::filesystem::path payload;
    if (argc > 1 && argv[1] && argv[1][0])
        payload = argv[1];
    else
        payload = std::filesystem::path(get_exe_directory()) / L"RobRun.sys";

    if (!NT_SUCCESS(intel_driver::load())) {
        util::log_error("Failed to load the mapper support driver");
        return 1;
    }
    util::log("Mapper support driver loaded");
    intel_driver::cache_trace_cleanup_context();
    intel_driver::perform_load_trace_cleanup(false);

    std::vector<uint8_t> image;
    if (!util::read_file_to_memory(payload.wstring(), &image) || image.empty()) {
        util::log_error("Cannot read payload: " + payload.string());
        intel_driver::unload();
        return 1;
    }

    util::log("Mapping payload: " + payload.filename().string());
    NTSTATUS exit_code = 0;
    const ULONG64 mapped_base = mapper::map_driver(
        image.data(), 0, 0, false, true,
        mapper::allocation_mode::allocate_pool, false, &exit_code);
    if (!mapped_base) {
        util::log_error("Payload mapping failed");
        intel_driver::unload();
        return 1;
    }

    std::ostringstream result;
    result << "Payload entry returned " << util::ntstatus_to_string(exit_code)
           << " (0x" << std::hex << exit_code << std::dec << ')';
    util::log(result.str());

    intel_driver::cache_trace_cleanup_context();
    intel_driver::perform_load_trace_cleanup(false);
    const NTSTATUS unload_status = intel_driver::unload();
    if (!NT_SUCCESS(unload_status)) {
        std::ostringstream message;
        message << "Mapper support driver unload failed (0x" << std::hex
                << unload_status << std::dec << ')';
        util::log_error(message.str());
    } else {
        util::log("Mapper support driver unloaded");
    }
    return 0;
}

std::wstring get_exe_directory(void) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}
