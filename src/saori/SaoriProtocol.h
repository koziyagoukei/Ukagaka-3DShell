#pragma once

// SAORI/1.0 (SAORI-universal) request/response text handling.
//
//   EXECUTE SAORI/1.0\r\n            GET Version SAORI/1.0\r\n
//   Charset: UTF-8\r\n               ...
//   Sender: <ghost>\r\n
//   SecurityLevel: Local\r\n
//   Argument0: <command>\r\n
//   Argument1: ...\r\n
//   \r\n
//
//   SAORI/1.0 200 OK\r\n
//   Charset: UTF-8\r\n
//   Result: <value>\r\n
//   Value0: <value>\r\n ...
//   \r\n

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uka3d::saori {

enum class Charset { utf8, shiftJis };

struct SaoriRequest {
    enum class Kind { execute, getVersion };
    Kind kind{Kind::execute};
    Charset charset{Charset::utf8};
    std::string charsetName{"UTF-8"}; // echoed in the response
    std::string sender;
    std::string securityLevel;
    std::vector<std::string> arguments; // UTF-8
};

struct SaoriResponse {
    int status{200};
    std::string result;                // UTF-8
    std::vector<std::string> values;   // UTF-8
};

// Parses raw request bytes. The Charset header selects the decoding (UTF-8 or
// Shift_JIS/CP932); without it UTF-8 is assumed when the bytes are valid UTF-8.
std::optional<SaoriRequest> parseRequest(std::string_view raw, std::string &error);

// Builds the response bytes in the request's charset. CR/LF inside values are
// replaced with spaces (they would break the header format).
std::string formatResponse(const SaoriResponse &response, Charset charset, std::string_view charsetName);

const char *statusText(int status);

} // namespace uka3d::saori
