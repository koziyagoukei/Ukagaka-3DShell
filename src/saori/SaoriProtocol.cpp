#include "saori/SaoriProtocol.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace uka3d::saori {
namespace {

std::string toLower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

bool isValidUtf8(std::string_view text) {
    return text.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

std::string convert(std::string_view text, UINT from, UINT to) {
    if (text.empty())
        return {};
    const int wideLength = MultiByteToWideChar(from, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(wideLength, 0)), L'\0');
    MultiByteToWideChar(from, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLength);
    const int length =
        WideCharToMultiByte(to, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(length, 0)), '\0');
    WideCharToMultiByte(to, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr);
    return result;
}

// Finds the Charset header in the raw bytes (header names are ASCII).
std::optional<std::string> findCharset(std::string_view raw) {
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t end = raw.find('\n', pos);
        if (end == std::string_view::npos)
            end = raw.size();
        std::string_view line = raw.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && toLower(trim(line.substr(0, colon))) == "charset")
            return std::string(trim(line.substr(colon + 1)));
        pos = end + 1;
    }
    return std::nullopt;
}

std::string sanitize(std::string_view text) {
    std::string result(text);
    std::ranges::replace(result, '\r', ' ');
    std::ranges::replace(result, '\n', ' ');
    return result;
}

} // namespace

const char *statusText(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    default:
        return "Internal Server Error";
    }
}

std::optional<SaoriRequest> parseRequest(std::string_view raw, std::string &error) {
    SaoriRequest request;
    if (const auto charset = findCharset(raw)) {
        const std::string lower = toLower(*charset);
        if (lower == "utf-8" || lower == "utf8") {
            request.charset = Charset::utf8;
        } else if (lower == "shift_jis" || lower == "shift-jis" || lower == "sjis" || lower == "windows-31j" ||
                   lower == "cp932" || lower == "x-sjis") {
            request.charset = Charset::shiftJis;
        } else {
            error = "未対応の Charset です: " + *charset;
            return std::nullopt;
        }
        request.charsetName = *charset;
    } else if (!isValidUtf8(raw)) {
        request.charset = Charset::shiftJis;
        request.charsetName = "Shift_JIS";
    }
    const std::string text = request.charset == Charset::shiftJis ? convert(raw, 932, CP_UTF8) : std::string(raw);
    if (request.charset == Charset::utf8 && !isValidUtf8(text)) {
        error = "要求が UTF-8 として不正です";
        return std::nullopt;
    }

    std::size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string_view line(text.data() + pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        pos = end + 1;
        if (first) {
            first = false;
            const std::string lower = toLower(trim(line));
            if (lower == "execute saori/1.0") {
                request.kind = SaoriRequest::Kind::execute;
            } else if (lower == "get version saori/1.0") {
                request.kind = SaoriRequest::Kind::getVersion;
            } else {
                error = "要求行が SAORI/1.0 ではありません";
                return std::nullopt;
            }
            continue;
        }
        if (line.empty())
            break; // end of headers
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        const std::string key = toLower(trim(line.substr(0, colon)));
        std::string_view value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1); // "Key: value" -> keep the value verbatim otherwise
        if (key == "sender") {
            request.sender = value;
        } else if (key == "securitylevel") {
            request.securityLevel = trim(value);
        } else if (key.starts_with("argument")) {
            int index = -1;
            const auto digits = std::string_view(key).substr(8);
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (ec != std::errc{} || ptr != digits.data() + digits.size() || index < 0 || index > 255)
                continue;
            if (request.arguments.size() <= static_cast<std::size_t>(index))
                request.arguments.resize(static_cast<std::size_t>(index) + 1);
            request.arguments[static_cast<std::size_t>(index)] = value;
        }
    }
    if (first) {
        error = "空の要求です";
        return std::nullopt;
    }
    return request;
}

std::string formatResponse(const SaoriResponse &response, Charset charset, std::string_view charsetName) {
    std::string text = "SAORI/1.0 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\n";
    text += "Charset: " + std::string(charsetName) + "\r\n";
    if (response.status == 200) {
        text += "Result: " + sanitize(response.result) + "\r\n";
        for (std::size_t i = 0; i < response.values.size(); ++i)
            text += "Value" + std::to_string(i) + ": " + sanitize(response.values[i]) + "\r\n";
    } else if (!response.result.empty()) {
        // Non-standard but harmless: lets scripts show why a request was rejected.
        text += "Result: " + sanitize(response.result) + "\r\n";
    }
    text += "\r\n";
    return charset == Charset::shiftJis ? convert(text, CP_UTF8, 932) : text;
}

} // namespace uka3d::saori
