#include "core/Http.h"

#include "core/Origin.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <system_error>

namespace eluvian
{

namespace
{

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view Reason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Status";
    }
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string_view HttpRequest::Header(std::string_view name) const
{
    for (const auto &[key, value] : headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

HttpResponse HttpResponse::OfJson(int status, const Json &body)
{
    return HttpResponse{status, "application/json", body.dump(), {}};
}

std::string Serialize(const HttpResponse &response, bool keepAlive)
{
    std::string out = std::format("HTTP/1.1 {} {}\r\n", response.status, Reason(response.status));
    if (!response.body.empty())
    {
        out += std::format("Content-Type: {}\r\n", response.contentType);
    }
    out += std::format("Content-Length: {}\r\n", response.body.size());
    out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto &[name, value] : response.headers)
    {
        out += std::format("{}: {}\r\n", name, value);
    }
    out += "\r\n";
    out += response.body;
    return out;
}

std::string UrlDecode(std::string_view text, bool query)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size())
        {
            const int high = HexDigit(text[i + 1]);
            const int low = HexDigit(text[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char>((high * 16) + low);
                i += 2;
                continue;
            }
        }
        out += (query && c == '+') ? ' ' : c;
    }
    return out;
}

HttpParser::Status HttpParser::Fail(int status, std::string message)
{
    errorStatus_ = status;
    errorMessage_ = std::move(message);
    return Status::Error;
}

HttpResponse HttpParser::ErrorResponse() const
{
    return HttpResponse::OfJson(errorStatus_, Json{{"error", errorMessage_}});
}

HttpParser::Status HttpParser::Feed(std::string_view bytes)
{
    buffer_.append(bytes);
    return Parse();
}

HttpParser::Status HttpParser::Parse()
{
    if (errorStatus_ != 0)
    {
        return Status::Error;
    }
    const auto headEnd = buffer_.find("\r\n\r\n");
    if (headEnd == std::string::npos)
    {
        return buffer_.size() > kMaxHead ? Fail(431, "the request's head is too long") : Status::NeedMore;
    }
    if (headEnd > kMaxHead)
    {
        return Fail(431, "the request's head is too long");
    }
    request_ = HttpRequest{};
    const std::string_view head(buffer_.data(), headEnd);
    const auto lineEnd = std::min(head.find("\r\n"), head.size());
    if (const auto status = ParseRequestLine(head.substr(0, lineEnd)); status != Status::Complete)
    {
        return status;
    }
    if (const auto status = ParseHeaders(lineEnd < head.size() ? head.substr(lineEnd + 2) : std::string_view());
        status != Status::Complete)
    {
        return status;
    }
    const std::size_t bodyStart = headEnd + 4;
    if (Lower(request_.Header("transfer-encoding")).find("chunked") != std::string::npos)
    {
        return ParseChunked(bodyStart);
    }
    return ParseBody(bodyStart);
}

// The request line: METHOD SP target SP HTTP/1.x, the target's query
// taken apart.
HttpParser::Status HttpParser::ParseRequestLine(std::string_view line)
{
    const auto space1 = line.find(' ');
    const auto space2 = space1 == std::string_view::npos ? space1 : line.find(' ', space1 + 1);
    if (space2 == std::string_view::npos)
    {
        return Fail(400, "not an HTTP request line");
    }
    request_.method = std::string(line.substr(0, space1));
    const std::string_view target = line.substr(space1 + 1, space2 - space1 - 1);
    const std::string_view version = line.substr(space2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0")
    {
        return Fail(400, "only HTTP/1.1 and 1.0 are spoken here");
    }
    http11_ = version == "HTTP/1.1";
    if (request_.method.empty() || target.empty() || target.front() != '/')
    {
        return Fail(400, "not an HTTP request line");
    }
    const auto queryAt = target.find('?');
    request_.path = UrlDecode(target.substr(0, queryAt), false);
    if (queryAt != std::string_view::npos)
    {
        std::string_view query = target.substr(queryAt + 1);
        while (!query.empty())
        {
            const auto amp = query.find('&');
            const std::string_view pair = query.substr(0, amp);
            const auto eq = pair.find('=');
            const auto key = UrlDecode(pair.substr(0, eq), true);
            if (!key.empty())
            {
                request_.query.emplace_back(key, eq == std::string_view::npos ? std::string()
                                                                              : UrlDecode(pair.substr(eq + 1), true));
            }
            query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        }
    }
    return Status::Complete;
}

// The header lines, and keep-alive by them and the version.
HttpParser::Status HttpParser::ParseHeaders(std::string_view lines)
{
    std::string_view rest = lines;
    while (!rest.empty())
    {
        const auto end = std::min(rest.find("\r\n"), rest.size());
        const std::string_view header = rest.substr(0, end);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0)
        {
            return Fail(400, "a header line without a name");
        }
        request_.headers.emplace_back(Lower(Trim(header.substr(0, colon))),
                                      std::string(Trim(header.substr(colon + 1))));
        rest = end < rest.size() ? rest.substr(end + 2) : std::string_view();
    }
    const auto connection = Lower(request_.Header("connection"));
    request_.keepAlive =
        http11_ ? connection.find("close") == std::string::npos : connection.find("keep-alive") != std::string::npos;
    return Status::Complete;
}

// A body of Content-Length bytes, or none.
HttpParser::Status HttpParser::ParseBody(std::size_t bodyStart)
{
    // 64 bits, since size_t is 32 in the game's build and a large length
    // would read as no number rather than as too large.
    std::uint64_t declared = 0;
    if (const auto text = request_.Header("content-length"); !text.empty())
    {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), declared);
        if (error == std::errc::result_out_of_range)
        {
            return Fail(413, "the body is larger than the bench takes (16 MB)");
        }
        if (error != std::errc{} || end != text.data() + text.size())
        {
            return Fail(400, "Content-Length is not a number");
        }
    }
    if (declared > kMaxBody)
    {
        return Fail(413, "the body is larger than the bench takes (16 MB)");
    }
    const auto length = static_cast<std::size_t>(declared);
    if (buffer_.size() < bodyStart + length)
    {
        return Status::NeedMore;
    }
    request_.body = buffer_.substr(bodyStart, length);
    buffer_.erase(0, bodyStart + length);
    return Status::Complete;
}

HttpParser::Status HttpParser::ParseChunked(std::size_t bodyStart)
{
    std::string body;
    std::size_t at = bodyStart;
    while (true)
    {
        const auto lineEnd = buffer_.find("\r\n", at);
        if (lineEnd == std::string::npos)
        {
            return buffer_.size() - at > kMaxHead ? Fail(400, "a chunk's size line is too long") : Status::NeedMore;
        }
        std::string_view sizeText(buffer_.data() + at, lineEnd - at);
        sizeText = Trim(sizeText.substr(0, sizeText.find(';')));
        std::size_t size = 0;
        const auto [end, error] = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size, 16);
        if (sizeText.empty() || error != std::errc{} || end != sizeText.data() + sizeText.size())
        {
            return Fail(400, "a chunk's size is not hexadecimal");
        }
        at = lineEnd + 2;
        if (size == 0)
        {
            return EndChunks(std::move(body), at);
        }
        if (body.size() + size > kMaxBody)
        {
            return Fail(413, "the body is larger than the bench takes (16 MB)");
        }
        if (buffer_.size() < at + size + 2)
        {
            return Status::NeedMore;
        }
        if (buffer_.compare(at + size, 2, "\r\n") != 0)
        {
            return Fail(400, "a chunk does not end where its size says");
        }
        body.append(buffer_, at, size);
        at += size + 2;
    }
}

// The last chunk's trailers, if any, end with an empty line; they are not
// kept.
HttpParser::Status HttpParser::EndChunks(std::string body, std::size_t at)
{
    const auto trailersEnd = buffer_.compare(at, 2, "\r\n") == 0 ? at : buffer_.find("\r\n\r\n", at);
    if (trailersEnd == std::string::npos)
    {
        return Status::NeedMore;
    }
    const auto consumed = trailersEnd == at ? at + 2 : trailersEnd + 4;
    request_.body = std::move(body);
    buffer_.erase(0, consumed);
    return Status::Complete;
}

HttpRequest HttpParser::Take()
{
    HttpRequest taken = std::move(request_);
    request_ = HttpRequest{};
    return taken;
}

HttpResponse Route(const HttpRequest &request, McpEndpoint &mcp, RestEndpoint &rest)
{
    if (!IsLocalOrigin(request.Header("origin")))
    {
        return HttpResponse::OfJson(403, Json{{"error", "the bench answers local clients only"}});
    }
    if (request.path == "/mcp")
    {
        if (request.method != "POST")
        {
            HttpResponse response = HttpResponse::OfJson(
                405, Json{{"error", "POST a JSON-RPC message to /mcp; each is answered in its own response, and "
                                    "there is no stream to GET"}});
            response.headers.emplace_back("Allow", "POST");
            return response;
        }
        const Json message = Json::parse(request.body, nullptr, false);
        if (message.is_discarded())
        {
            return HttpResponse::OfJson(400, McpEndpoint::ParseError("the body is not JSON"));
        }
        const auto reply = mcp.Handle(message);
        return reply ? HttpResponse::OfJson(200, *reply) : HttpResponse{202, "application/json", {}, {}};
    }
    if (request.path.starts_with("/api/"))
    {
        const auto answer = rest.Handle(request.method, request.path, request.query, request.body);
        return HttpResponse::OfJson(answer.status, answer.body);
    }
    return HttpResponse::OfJson(404, Json{{"error", "MCP is at POST /mcp; REST at /api/ (GET /api/tools lists "
                                                    "the tools)"}});
}

} // namespace eluvian
