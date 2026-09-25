// File: src/core_types.cppm
export module kernel86.types;

export namespace kernel86 {
    using uint8_t   = unsigned char;
    using uint16_t  = unsigned short;
    using uint32_t  = unsigned int;
    using uint64_t  = unsigned long long;
    using uintptr_t = unsigned long long;
    using size_t    = unsigned long long;

    enum class [[nodiscard]] Status : uint8_t {
        Success = 0,
        OutOfMemory,
        InvalidCapability,
        InvalidArgument
    };
}
