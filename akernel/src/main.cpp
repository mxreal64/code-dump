// File: src/main.cpp
import kernel86.types;
import kernel86.terminal;
import kernel86.mm;

// --- THE CRITICAL HARDWARE COMPILER BRIDGE ---
// This prevents GCC from stalling the processor on internal structural setups
extern "C" void* memset(void* dest, int ch, kernel86::size_t count) noexcept {
    auto* ptr = static_cast<unsigned char*>(dest);
    for (kernel86::size_t i = 0; i < count; ++i) {
        ptr[i] = static_cast<unsigned char>(ch);
    }
    return dest;
}

kernel86::terminal::Console sys_console;
kernel86::mm::MemoryManager sys_mm;

extern "C" void kernel_main() noexcept {
    // Claim the screen layout immediately
    sys_console.SetColor(kernel86::terminal::Color::LightCyan, kernel86::terminal::Color::Black);
    sys_console.Clear();

    sys_console.Write("========================================\n");
    sys_console.Write("          Welcome to Kernel86           \n");
    sys_console.Write("========================================\n\n");
    sys_console.Write("[OK] Core 0 reached kernel_main successfully.\n");

    // Initialize memory structures and check allocations
    sys_console.Write("[INFO] Allocating secure memory object...\n");
    auto [status, user_cap] = sys_mm.AllocateObject(32, static_cast<kernel86::mm::CapRights>(
        static_cast<kernel86::uint8_t>(kernel86::mm::CapRights::Read) | static_cast<kernel86::uint8_t>(kernel86::mm::CapRights::Write)
    ));

    if (status == kernel86::Status::Success) {
        sys_console.SetColor(kernel86::terminal::Color::LightGreen, kernel86::terminal::Color::Black);
        sys_console.Write("[OK] Secure object registry complete.\n");
    }

    // Verify type-safe translate pathways
    auto [trans_status, phys_addr] = sys_mm.Translate(user_cap, kernel86::mm::CapRights::Write);
    if (trans_status == kernel86::Status::Success) {
        sys_console.Write("[OK] Capability address verification passed.\n");
    }

    // Run the capability access security validation test
    sys_console.SetColor(kernel86::terminal::Color::LightCyan, kernel86::terminal::Color::Black);
    sys_console.Write("[INFO] Attempting illegal Execution access test...\n");

    auto [bad_status, bad_addr] = sys_mm.Translate(user_cap, kernel86::mm::CapRights::Exec);
    if (bad_status == kernel86::Status::InvalidCapability) {
        sys_console.SetColor(kernel86::terminal::Color::LightRed, kernel86::terminal::Color::Black);
        sys_console.Write("[FAIL] Blocked execution privilege escalation attack.\n");
    }

    sys_console.SetColor(kernel86::terminal::Color::White, kernel86::terminal::Color::Black);
    sys_console.Write("\nKernel86 system state: STABLE. Standing by.\n");

    while (true) {
        asm volatile("hlt");
    }
}
