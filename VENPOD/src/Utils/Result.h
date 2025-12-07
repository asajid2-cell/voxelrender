#pragma once

#include <string>
#include <variant>
#include <utility>
#include <fmt/format.h>

namespace VENPOD {

// Modern C++20 error handling - simplified std::expected alternative
template<typename T, typename E = std::string>
class Result {
private:
    // Wrapper to disambiguate when T and E are the same type
    struct ErrorWrapper {
        E error;
        explicit ErrorWrapper(E e) : error(std::move(e)) {}
    };

public:
    // Success constructor
    static Result Ok(T value) {
        return Result(std::move(value), true);
    }

    // Error constructor
    static Result Err(E error) {
        return Result(ErrorWrapper(std::move(error)), false);
    }

    // Check if result contains a value
    [[nodiscard]] bool IsOk() const { return std::holds_alternative<T>(m_data); }
    [[nodiscard]] bool IsErr() const { return std::holds_alternative<ErrorWrapper>(m_data); }

    // Boolean conversion and operator!
    [[nodiscard]] explicit operator bool() const { return IsOk(); }
    [[nodiscard]] bool operator!() const { return IsErr(); }

    // Access value (throws if error)
    [[nodiscard]] T& Value() & { return std::get<T>(m_data); }
    [[nodiscard]] const T& Value() const& { return std::get<T>(m_data); }
    [[nodiscard]] T&& Value() && { return std::move(std::get<T>(m_data)); }

    // Lowercase aliases for compatibility
    [[nodiscard]] T& value() & { return Value(); }
    [[nodiscard]] const T& value() const& { return Value(); }
    [[nodiscard]] T&& value() && { return std::move(Value()); }

    // Access error (throws if ok)
    [[nodiscard]] E& Error() & { return std::get<ErrorWrapper>(m_data).error; }
    [[nodiscard]] const E& Error() const& { return std::get<ErrorWrapper>(m_data).error; }

    // Lowercase alias for compatibility
    [[nodiscard]] E& error() & { return Error(); }
    [[nodiscard]] const E& error() const& { return Error(); }

    // Safe access with default
    [[nodiscard]] T ValueOr(T default_value) const& {
        return IsOk() ? Value() : std::move(default_value);
    }

private:
    Result() = default;
    explicit Result(T value, bool) : m_data(std::move(value)) {}
    explicit Result(ErrorWrapper error, bool) : m_data(std::move(error)) {}

    std::variant<T, ErrorWrapper> m_data;
};

// Specialization for void success type
template<typename E>
class Result<void, E> {
public:
    // Default constructor creates success
    Result() : m_ok(true) {}

    static Result Ok() { return Result(true); }
    static Result Err(E error) {
        Result r(false);
        r.m_error = std::move(error);
        return r;
    }

    [[nodiscard]] bool IsOk() const { return m_ok; }
    [[nodiscard]] bool IsErr() const { return !m_ok; }

    // Boolean conversion and operator!
    [[nodiscard]] explicit operator bool() const { return m_ok; }
    [[nodiscard]] bool operator!() const { return !m_ok; }

    [[nodiscard]] const E& Error() const { return m_error; }

    // Lowercase alias for compatibility
    [[nodiscard]] const E& error() const { return m_error; }

private:
    explicit Result(bool ok) : m_ok(ok) {}

    bool m_ok;
    E m_error;
};

// Helper function to create error Results with formatted messages
template<typename... Args>
Result<void> Error(fmt::format_string<Args...> fmt, Args&&... args) {
    return Result<void>::Err(fmt::format(fmt, std::forward<Args>(args)...));
}

// Helper for non-void Results
template<typename T, typename... Args>
Result<T> MakeError(fmt::format_string<Args...> fmt, Args&&... args) {
    return Result<T>::Err(fmt::format(fmt, std::forward<Args>(args)...));
}

} // namespace VENPOD
