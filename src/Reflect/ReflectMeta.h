#ifndef FCT_REFLECT_REFLECTMETA_H
#define FCT_REFLECT_REFLECTMETA_H

#include <tuple>
#include <string_view>
#include <array>
#include <optional>
#include <type_traits>
#include <utility>
#include <cstddef>
#include <boost/pfr.hpp>
#include <absl/status/statusor.h>
#include <absl/status/status.h>

namespace FCT::Reflect {

    /**
     * @brief 字段数值范围元数据属性 [[mq::range(min, max)]]
     */
    struct FieldRange {
        bool enabled = false;
        double min = 0.0;
        double max = 0.0;
    };

    /**
     * @brief 成员数据指针特征提取
     */
    template<typename PtrType>
    struct MemberPointerTraits;

    template<typename C, typename V>
    struct MemberPointerTraits<V C::*> {
        using ClassType = C;
        using ValueType = V;
    };

    /**
     * @brief 成员函数指针特征提取（覆盖 const 与 noexcept 修饰）
     */
    template<typename T>
    struct MethodTraits;

    template<typename C, typename R, typename... Args>
    struct MethodTraits<R (C::*)(Args...)> {
        using ClassType = C;
        using ReturnType = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr std::size_t arity = sizeof...(Args);
        static constexpr bool isConst = false;
        static constexpr bool isNoexcept = false;
    };

    template<typename C, typename R, typename... Args>
    struct MethodTraits<R (C::*)(Args...) const> {
        using ClassType = C;
        using ReturnType = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr std::size_t arity = sizeof...(Args);
        static constexpr bool isConst = true;
        static constexpr bool isNoexcept = false;
    };

    template<typename C, typename R, typename... Args>
    struct MethodTraits<R (C::*)(Args...) noexcept> {
        using ClassType = C;
        using ReturnType = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr std::size_t arity = sizeof...(Args);
        static constexpr bool isConst = false;
        static constexpr bool isNoexcept = true;
    };

    template<typename C, typename R, typename... Args>
    struct MethodTraits<R (C::*)(Args...) const noexcept> {
        using ClassType = C;
        using ReturnType = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr std::size_t arity = sizeof...(Args);
        static constexpr bool isConst = true;
        static constexpr bool isNoexcept = true;
    };

    /**
     * @brief 编译期字段元数据描述符
     */
    template<typename MemberPtrType>
    struct FieldMeta {
        using Traits = MemberPointerTraits<MemberPtrType>;
        using ClassType = typename Traits::ClassType;
        using ValueType = typename Traits::ValueType;
        using PointerType = MemberPtrType;

        std::string_view name;
        MemberPtrType ptr;
        FieldRange range{};
        bool readOnly = false;
        std::string_view tooltip{};

        constexpr FieldMeta(
            std::string_view n,
            MemberPtrType p,
            FieldRange r = {},
            bool ro = false,
            std::string_view tt = {}) noexcept
            : name(n), ptr(p), range(r), readOnly(ro), tooltip(tt) {}
    };

    template<typename MemberPtrType>
    constexpr auto makeFieldMeta(
        std::string_view name,
        MemberPtrType ptr,
        FieldRange range = {},
        bool readOnly = false,
        std::string_view tooltip = {}) noexcept {
        return FieldMeta<MemberPtrType>(name, ptr, range, readOnly, tooltip);
    }

    /**
     * @brief 编译期成员函数元数据描述符（包含精确参数名称数组）
     */
    template<typename MethodPtrType, std::size_t Arity>
    struct MethodMeta {
        using Traits = MethodTraits<MethodPtrType>;
        using ClassType = typename Traits::ClassType;
        using ReturnType = typename Traits::ReturnType;
        using ArgsTuple = typename Traits::ArgsTuple;
        using PointerType = MethodPtrType;
        static constexpr std::size_t arity = Arity;

        std::string_view name;
        MethodPtrType ptr;
        std::array<std::string_view, Arity> paramNames;
        bool isScript = true;
        std::string_view tooltip{};

        constexpr MethodMeta(
            std::string_view n,
            MethodPtrType p,
            std::array<std::string_view, Arity> params,
            bool script = true,
            std::string_view tt = {}) noexcept
            : name(n), ptr(p), paramNames(params), isScript(script), tooltip(tt) {}
    };

    template<typename MethodPtrType, std::size_t N>
    constexpr auto makeMethodMeta(
        std::string_view name,
        MethodPtrType ptr,
        const std::array<std::string_view, N>& paramNames,
        bool isScript = true,
        std::string_view tooltip = {}) noexcept {
        static_assert(N == MethodTraits<MethodPtrType>::arity,
            "Param names array size must match method arity");
        return MethodMeta<MethodPtrType, N>(name, ptr, paramNames, isScript, tooltip);
    }

    /**
     * @brief 编译期枚举项元数据描述符
     */
    template<typename EnumType>
    struct EnumItemMeta {
        using Type = EnumType;
        EnumType value;
        std::string_view name;

        constexpr EnumItemMeta(EnumType val, std::string_view n) noexcept
            : value(val), name(n) {}
    };

    template<typename EnumType>
    constexpr auto makeEnumItemMeta(EnumType val, std::string_view name) noexcept {
        return EnumItemMeta<EnumType>(val, name);
    }

    /**
     * @brief 类型元数据主模板（由 mq_reflect 进行特化）
     */
    template<typename T>
    struct TypeMeta;

    template<typename T, typename = void>
    struct HasStaticTypeMetaHelper : std::false_type {};

    template<typename T>
    struct HasStaticTypeMetaHelper<T, std::void_t<decltype(TypeMeta<std::decay_t<T>>::name)>> : std::true_type {};

    template<typename T>
    inline constexpr bool HasStaticTypeMeta = HasStaticTypeMetaHelper<T>::value;

    template<typename T>
    concept ReflectableType = HasStaticTypeMeta<T>;

    template<typename T>
    inline constexpr bool HasStaticFields = HasStaticTypeMeta<T> && requires {
        TypeMeta<std::decay_t<T>>::fields;
        TypeMeta<std::decay_t<T>>::fieldCount;
    };

    template<typename T>
    inline constexpr bool HasStaticMethods = HasStaticTypeMeta<T> && requires {
        TypeMeta<std::decay_t<T>>::methods;
        TypeMeta<std::decay_t<T>>::methodCount;
    };

    template<typename T>
    inline constexpr bool HasStaticEnums = HasStaticTypeMeta<T> && requires {
        TypeMeta<std::decay_t<T>>::enumerators;
        TypeMeta<std::decay_t<T>>::enumCount;
    };

    /**
     * @brief 检测类型是否可通过 Boost.PFR 进行零宏聚合体反射
     */
    template<typename T>
    constexpr bool is_pfr_reflectable_v =
        !HasStaticTypeMeta<T> &&
        std::is_aggregate_v<std::decay_t<T>> &&
        !std::is_union_v<std::decay_t<T>> &&
        !std::is_array_v<std::remove_reference_t<T>> &&
        !std::is_empty_v<std::decay_t<T>>;

    template<typename T>
    concept HasPfrFallback = is_pfr_reflectable_v<T>;

    /**
     * @brief MSVC C++20 字段结构化标签类型（空结构体，零运行时开销）
     */
    template<typename T, std::size_t I>
    struct FieldConst {
        using ClassType = std::decay_t<T>;
        static constexpr const auto& meta = std::get<I>(TypeMeta<ClassType>::fields);
        using ValueType = typename std::remove_cvref_t<decltype(meta)>::ValueType;
        using PointerType = typename std::remove_cvref_t<decltype(meta)>::PointerType;

        static constexpr std::size_t index = I;
        static constexpr auto ptr = meta.ptr;
        static constexpr std::string_view name = meta.name;
        static constexpr auto range = meta.range;
        static constexpr bool readOnly = meta.readOnly;
        static constexpr std::string_view tooltip = meta.tooltip;

        template<typename Obj>
        constexpr decltype(auto) operator()(Obj&& obj) const noexcept {
            return std::forward<Obj>(obj).*ptr;
        }
        template<typename Obj>
        constexpr decltype(auto) get(Obj&& obj) const noexcept {
            return std::forward<Obj>(obj).*ptr;
        }
        constexpr operator std::string_view() const noexcept { return name; }
    };

    /**
     * @brief Boost.PFR 字段结构化标签类型（对齐 FieldConst 接口）
     */
    template<typename T, std::size_t I>
    struct PfrFieldConst {
        using ClassType = std::decay_t<T>;
        using ValueType = boost::pfr::tuple_element_t<I, ClassType>;

        static constexpr std::size_t index = I;
        static constexpr std::string_view name = boost::pfr::get_name<I, ClassType>();
        static constexpr FieldRange range{false, 0.0, 0.0};
        static constexpr bool readOnly = false;
        static constexpr std::string_view tooltip{};

        template<typename Obj>
        constexpr decltype(auto) operator()(Obj&& obj) const noexcept {
            return boost::pfr::get<I>(std::forward<Obj>(obj));
        }
        template<typename Obj>
        constexpr decltype(auto) get(Obj&& obj) const noexcept {
            return boost::pfr::get<I>(std::forward<Obj>(obj));
        }
        constexpr operator std::string_view() const noexcept { return name; }
    };

    /**
     * @brief MSVC C++20 方法结构化标签类型
     */
    template<typename T, std::size_t I>
    struct MethodConst {
        using ClassType = std::decay_t<T>;
        static constexpr const auto& meta = std::get<I>(TypeMeta<ClassType>::methods);
        using Traits = typename std::remove_cvref_t<decltype(meta)>::Traits;
        using ReturnType = typename Traits::ReturnType;
        using ArgsTuple = typename Traits::ArgsTuple;
        using PointerType = typename std::remove_cvref_t<decltype(meta)>::PointerType;
        static constexpr std::size_t arity = std::remove_cvref_t<decltype(meta)>::arity;

        static constexpr std::size_t index = I;
        static constexpr auto ptr = meta.ptr;
        static constexpr std::string_view name = meta.name;
        static constexpr auto paramNames = meta.paramNames;
        static constexpr bool isScript = meta.isScript;
        static constexpr std::string_view tooltip = meta.tooltip;

        template<typename Instance, typename... Args>
        constexpr decltype(auto) operator()(Instance* inst, Args&&... args) const {
            return (inst->*ptr)(std::forward<Args>(args)...);
        }
        template<typename Instance, typename... Args>
        constexpr decltype(auto) operator()(Instance& inst, Args&&... args) const {
            return (inst.*ptr)(std::forward<Args>(args)...);
        }
        template<typename Instance, typename... Args>
        constexpr decltype(auto) invoke(Instance&& inst, Args&&... args) const {
            return (std::forward<Instance>(inst).*ptr)(std::forward<Args>(args)...);
        }
    };

    /**
     * @brief MSVC C++20 枚举项结构化标签类型
     */
    template<typename E, std::size_t I>
    struct EnumConst {
        using EnumType = std::decay_t<E>;
        static constexpr const auto& meta = std::get<I>(TypeMeta<EnumType>::enumerators);

        static constexpr std::size_t index = I;
        static constexpr EnumType value = meta.value;
        static constexpr std::string_view name = meta.name;

        constexpr operator EnumType() const noexcept { return value; }
    };

    /**
     * @brief 类型列表容器与统一 templateFor 展开
     */
    template<typename... Ts>
    struct TypeList {
        static constexpr std::size_t size = sizeof...(Ts);
    };

    template<typename... Ts, typename Fn>
    constexpr void templateFor(TypeList<Ts...>, Fn&& fn) {
        if constexpr (sizeof...(Ts) > 0) {
            using First = std::tuple_element_t<0, std::tuple<Ts...>>;
            if constexpr (requires { fn.template operator()<First{}>(); }) {
                (fn.template operator()<Ts{}>(), ...);
            } else if constexpr (requires { fn(First{}); }) {
                (fn(Ts{}), ...);
            } else if constexpr (requires { fn.template operator()<First>(); }) {
                (fn.template operator()<Ts>(), ...);
            }
        }
    }

    template<typename TypeListType, typename Fn>
    constexpr void templateFor(Fn&& fn) {
        templateFor(TypeListType{}, std::forward<Fn>(fn));
    }

    /**
     * @brief 编译期字段循环展开（支持静态 TypeMeta 与 Boost.PFR 回退）
     */
    template<typename T, typename Fn>
    constexpr void templateForFields(Fn&& fn) {
        using Decayed = std::decay_t<T>;
        if constexpr (HasStaticFields<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::fieldCount;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    if constexpr (requires { fn.template operator()<FieldConst<Decayed, 0>{}>(); }) {
                        (fn.template operator()<FieldConst<Decayed, Is>{}>(), ...);
                    } else if constexpr (requires { fn(FieldConst<Decayed, 0>{}); }) {
                        (fn(FieldConst<Decayed, Is>{}), ...);
                    } else if constexpr (requires { fn.template operator()<FieldConst<Decayed, 0>>(); }) {
                        (fn.template operator()<FieldConst<Decayed, Is>>(), ...);
                    }
                }(std::make_index_sequence<count>{});
            }
        } else if constexpr (is_pfr_reflectable_v<Decayed>) {
            constexpr std::size_t count = boost::pfr::tuple_size_v<Decayed>;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    if constexpr (requires { fn.template operator()<PfrFieldConst<Decayed, 0>{}>(); }) {
                        (fn.template operator()<PfrFieldConst<Decayed, Is>{}>(), ...);
                    } else if constexpr (requires { fn(PfrFieldConst<Decayed, 0>{}); }) {
                        (fn(PfrFieldConst<Decayed, Is>{}), ...);
                    } else if constexpr (requires { fn.template operator()<PfrFieldConst<Decayed, 0>>(); }) {
                        (fn.template operator()<PfrFieldConst<Decayed, Is>>(), ...);
                    }
                }(std::make_index_sequence<count>{});
            }
        }
    }

    /**
     * @brief 编译期方法循环展开
     */
    template<typename T, typename Fn>
    constexpr void templateForMethods(Fn&& fn) {
        using Decayed = std::decay_t<T>;
        if constexpr (HasStaticMethods<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::methodCount;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    if constexpr (requires { fn.template operator()<MethodConst<Decayed, 0>{}>(); }) {
                        (fn.template operator()<MethodConst<Decayed, Is>{}>(), ...);
                    } else if constexpr (requires { fn(MethodConst<Decayed, 0>{}); }) {
                        (fn(MethodConst<Decayed, Is>{}), ...);
                    } else if constexpr (requires { fn.template operator()<MethodConst<Decayed, 0>>(); }) {
                        (fn.template operator()<MethodConst<Decayed, Is>>(), ...);
                    }
                }(std::make_index_sequence<count>{});
            }
        }
    }

    /**
     * @brief 编译期枚举项循环展开
     */
    template<typename E, typename Fn>
    constexpr void templateForEnums(Fn&& fn) {
        using Decayed = std::decay_t<E>;
        if constexpr (HasStaticEnums<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::enumCount;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    if constexpr (requires { fn.template operator()<EnumConst<Decayed, 0>{}>(); }) {
                        (fn.template operator()<EnumConst<Decayed, Is>{}>(), ...);
                    } else if constexpr (requires { fn(EnumConst<Decayed, 0>{}); }) {
                        (fn(EnumConst<Decayed, Is>{}), ...);
                    } else if constexpr (requires { fn.template operator()<EnumConst<Decayed, 0>>(); }) {
                        (fn.template operator()<EnumConst<Decayed, Is>>(), ...);
                    }
                }(std::make_index_sequence<count>{});
            }
        }
    }

    /**
     * @brief 值标签字段遍历（统一 TypeMeta 与 Boost.PFR）
     */
    template<typename T, typename Fn>
    constexpr void forEachField(T&& obj, Fn&& fn) {
        using Decayed = std::decay_t<T>;
        if constexpr (HasStaticFields<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::fieldCount;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    (fn(FieldConst<Decayed, Is>{}, std::forward<T>(obj).*(FieldConst<Decayed, Is>::ptr)), ...);
                }(std::make_index_sequence<count>{});
            }
        } else if constexpr (is_pfr_reflectable_v<Decayed>) {
            constexpr std::size_t count = boost::pfr::tuple_size_v<Decayed>;
            if constexpr (count > 0) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    (fn(PfrFieldConst<Decayed, Is>{}, boost::pfr::get<Is>(std::forward<T>(obj))), ...);
                }(std::make_index_sequence<count>{});
            }
        }
    }

    template<typename T, typename Fn>
    constexpr void forEachMethod(Fn&& fn) {
        templateForMethods<T>(std::forward<Fn>(fn));
    }

    template<typename E, typename Fn>
    constexpr void forEachEnum(Fn&& fn) {
        templateForEnums<E>(std::forward<Fn>(fn));
    }

    /**
     * @brief 编译期枚举转字符串
     */
    template<typename E>
    constexpr std::string_view enumToString(E val) noexcept {
        using Decayed = std::decay_t<E>;
        static_assert(std::is_enum_v<Decayed>, "enumToString requires an enum type");
        if constexpr (HasStaticEnums<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::enumCount;
            std::string_view result = "";
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (
                    ((std::get<Is>(TypeMeta<Decayed>::enumerators).value == val)
                        ? (result = std::get<Is>(TypeMeta<Decayed>::enumerators).name, true)
                        : false) || ...
                );
            }(std::make_index_sequence<count>{});
            return result;
        } else {
            return "";
        }
    }

    /**
     * @brief 编译期字符串转枚举
     */
    template<typename E>
    constexpr std::optional<E> stringToEnum(std::string_view str) noexcept {
        using Decayed = std::decay_t<E>;
        static_assert(std::is_enum_v<Decayed>, "stringToEnum requires an enum type");
        if constexpr (HasStaticEnums<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::enumCount;
            std::optional<Decayed> result = std::nullopt;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (
                    ((std::get<Is>(TypeMeta<Decayed>::enumerators).name == str)
                        ? (result = std::get<Is>(TypeMeta<Decayed>::enumerators).value, true)
                        : false) || ...
                );
            }(std::make_index_sequence<count>{});
            return result;
        } else {
            return std::nullopt;
        }
    }

    /**
     * @brief 运行期安全枚举解析（遵守 StatusOr 错误规范）
     */
    template<typename E>
    inline absl::StatusOr<E> parseEnum(std::string_view str) {
        if (auto val = stringToEnum<E>(str)) {
            return *val;
        }
        return absl::NotFoundError("Unknown enum value '" + std::string(str) + "'");
    }

    /**
     * @brief 获取枚举所有名称数组
     */
    template<typename E>
    constexpr auto getEnumNames() {
        using Decayed = std::decay_t<E>;
        if constexpr (HasStaticEnums<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::enumCount;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::array<std::string_view, count>{
                    std::get<Is>(TypeMeta<Decayed>::enumerators).name...
                };
            }(std::make_index_sequence<count>{});
        } else {
            return std::array<std::string_view, 0>{};
        }
    }

    /**
     * @brief 获取枚举所有枚举常量数组
     */
    template<typename E>
    constexpr auto getEnumValues() {
        using Decayed = std::decay_t<E>;
        if constexpr (HasStaticEnums<Decayed>) {
            constexpr std::size_t count = TypeMeta<Decayed>::enumCount;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::array<Decayed, count>{
                    std::get<Is>(TypeMeta<Decayed>::enumerators).value...
                };
            }(std::make_index_sequence<count>{});
        } else {
            return std::array<Decayed, 0>{};
        }
    }

} // namespace FCT::Reflect

#endif // FCT_REFLECT_REFLECTMETA_H
