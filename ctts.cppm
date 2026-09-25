export module CompileTimeThreadSafety;

import <meta>;
import <type_traits>;
import <utility>;
import <cstddef>;

namespace ctts {

    template <typename ParentType, std::meta::info Member>
    consteval bool IsFieldThreadSafe() noexcept {
        using FieldType = typename [: std::meta::type_of(Member) :];
        using CleanField = std::decay_t<FieldType>;

        if constexpr (requires { typename ParentType::is_synchronized; }) {
            return true;
        }

        if constexpr (std::is_const_v<FieldType>) {
            return true;
        }

        if constexpr (std::is_volatile_v<FieldType> || std::is_pointer_v<FieldType>) {
            return false;
        }

        if constexpr (std::is_fundamental_v<CleanField>) {
            return false;
        }

        return true;
    }

    template <typename T>
    consteval bool AuditThreadSafety() noexcept {
        using CleanType = std::decay_t<T>;

        static constexpr auto static_members = std::define_static_array(
            std::meta::members_of(^^CleanType, std::meta::access_context::unchecked())
        );

        bool is_secure = true;

        template for (constexpr std::meta::info member : static_members) {
            if constexpr (!std::meta::is_function(member) && !std::meta::is_type(member)) {
                if constexpr (!IsFieldThreadSafe<CleanType, member>()) {
                    is_secure = false;
                }
            }
        }

        return is_secure;
    }

    export template <typename T>
    constexpr bool IsThreadSafe_v = AuditThreadSafety<T>();

    export template <typename T>
    struct IsThreadSafe : std::bool_constant<AuditThreadSafety<T>()> {};

    export template <typename T>
    class ThreadGuard {
    private:
        T instance_;

        static_assert(IsThreadSafe_v<T>,
                      "\nwhy? why did u do that?\n");

    public:
        template <typename... Args>
        constexpr explicit ThreadGuard(Args&&... args) noexcept
        : instance_(std::forward<Args>(args)...) {}

        [[nodiscard]] constexpr T* operator->() noexcept { return &instance_; }
        [[nodiscard]] constexpr const T* operator->() const noexcept { return &instance_; }
    };

} // namespace ctts
