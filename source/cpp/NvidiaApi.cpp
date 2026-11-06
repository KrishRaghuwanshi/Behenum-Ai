// language: C++, file: NvidiaApi.cpp, target: Windows 11, MSVC
// *Native Google AI Studio API streaming client (WinHTTP) - replaces javascript/nvidia-api.js
// *NO Electron, NO Node fetch, NO local weights. Runs inside the hollowed host
// *process (svchost identity), so the TLS connection originates from a system
// *process, not from electron.exe. Single model (gemini-3.5-flash-lite), same
// *SSE parsing, same image payloads as the Baihan-Ai provider.
// *Stealth: plain HTTPS POST, 3 minimal headers, generic UA, calls fire ONLY on
// *user click (button/hotkey), screenshots batched (max 6). Key buffer is owned
// *by the caller and must be zeroed after use (see ZeroApiKey below).

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

static constexpr const wchar_t GEMINI_HOST[] = L"generativelanguage.googleapis.com";
static constexpr const wchar_t GEMINI_PATH_PREFIX[] = L"/v1beta/models/";
static constexpr const wchar_t GEMINI_PATH_SUFFIX[] = L":streamGenerateContent?alt=sse&key=";
static constexpr const char* PRIMARY_MODEL = "gemini-3.5-flash-lite";

static constexpr const char* SYSTEM_PROMPT =
    "You are behenum-ai, a real-time mock test copilot and you are being used in a mock test. Produce concise, directly usable "
    "answers in clean Markdown. For theory, factual, behavioral, and conceptual questions, "
    "prefer short bullet points instead of long paragraphs , focus on whats being asked in the question and answer in short . For coding questions"
    ", see whats asked in question and only send the required answer- if its a code then put complete usable code in a fenced Markdown code block "
    "with the correct language tag and see if there is some error shown in image then reply thoroughly where is wrong and what to replace in code properly ";

// ---------- small JSON escaper (text + dataUrls into one JSON string) ----------
static void JsonEscape(const std::string& in, std::string& out) {
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                sprintf_s(b, "\\u%04x", c);
                out += b;
            } else out += c;
        }
    }
}

static int MaxTokensFor(const char* model) {
    return 8192; // gemini-3.5-flash-lite default
}

// ---------- screenshot payload (PNG dataUrls from ScreenCapture path) ----------
struct ApiScreenshot {
    std::string dataUrl; // "data:image/png;base64,...."
};

// ─── extern globals set by ChimeraMain.cpp before calling NvidiaStreamAnswer ──
// Read-only from this file.  StreamTrayToAi populates them, then calls us.
// History arrays are parallel (histUser[i] pairs with histAsst[i]).
// cancelFlag is set by UiActionCancel when the user clicks Stop.
extern const char* const* g_nvHistUser;   // may be nullptr
extern const char* const* g_nvHistAsst;   // may be nullptr
extern int                g_nvHistCount;  // 0 if no history
extern volatile bool      g_nvCancelStream;

// ---------- ORIGINAL 7-arg signature (unchanged from working version) ----------
bool NvidiaStreamAnswer(
    const char* apiKey,
    const char* userText,
    const ApiScreenshot* screens,
    int screenCount,
    const char* preferredModel,
    void (*onDelta)(const char* delta, size_t len, void* userCtx),
    void* userCtx)
{
    // Use primary model only - no fallback
    const char* model = PRIMARY_MODEL;

    // ---- build JSON body (Gemini format) ----
    std::string escText, body = "{\"contents\":[";
    
    // System instruction (Gemini uses systemInstruction field)
    body += "{\"role\":\"user\",\"parts\":[{\"text\":\"";
    JsonEscape(SYSTEM_PROMPT, escText);
    body += escText;
    body += "\"}]},{\"role\":\"model\",\"parts\":[{\"text\":\"Understood. I will provide concise, actionable answers.\"}]}";
    
    // History: read from extern globals set by ChimeraMain before this call
    for (int h = 0; h < g_nvHistCount; h++) {
        if (g_nvHistUser && g_nvHistUser[h]) {
            body += ",{\"role\":\"user\",\"parts\":[{\"text\":\"";
            escText.clear(); JsonEscape(g_nvHistUser[h], escText);
            body += escText; body += "\"}]}";
        }
        if (g_nvHistAsst && g_nvHistAsst[h]) {
            body += ",{\"role\":\"model\",\"parts\":[{\"text\":\"";
            escText.clear(); JsonEscape(g_nvHistAsst[h], escText);
            body += escText; body += "\"}]}";
        }
    }
    
    // Current user turn (may include screenshots)
    body += ",{\"role\":\"user\",\"parts\":[{\"text\":\"";
    escText.clear();
    JsonEscape(userText ? userText : "", escText);
    body += escText;
    body += "\"}";
    
    // Add inline images (Gemini uses inline_data format)
    for (int s = 0; s < screenCount; s++) {
        // Extract base64 data from dataUrl (remove "data:image/png;base64," prefix)
        std::string dataUrl = screens[s].dataUrl;
        size_t commaPos = dataUrl.find(',');
        if (commaPos != std::string::npos) {
            std::string base64Data = dataUrl.substr(commaPos + 1);
            body += ",{\"inline_data\":{\"mime_type\":\"image/png\",\"data\":\"";
            escText.clear();
            JsonEscape(base64Data, escText);
            body += escText;
            body += "\"}}";
        }
    }
    
    body += "]}],\"generationConfig\":{\"maxOutputTokens\":";
    body += std::to_string(MaxTokensFor(model));
    body += ",\"temperature\":0.7,\"topP\":0.95}}";

    // ---- HTTPS POST (generic UA, minimal headers) ----
    // Build path with model and API key: /v1beta/models/{model}:streamGenerateContent?alt=sse&key={apiKey}
    std::wstring requestPath = GEMINI_PATH_PREFIX;
    std::string modelStr(model);
    requestPath += std::wstring(modelStr.begin(), modelStr.end());
    requestPath += GEMINI_PATH_SUFFIX;
    std::string apiKeyStr(apiKey);
    requestPath += std::wstring(apiKeyStr.begin(), apiKeyStr.end());
    
    // WINHTTP_ACCESS_TYPE_NO_PROXY: avoids WPAD/PAC auto-detection.
    // From svchost (SYSTEM), default proxy detection spawns internal threads
    // outside ActionThread's __except scope that can crash with AV → svchost dies.
    bool streamDone = false;
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        SecureZeroMemory(const_cast<char*>(body.data()), body.size());
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, GEMINI_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        hRequest = WinHttpOpenRequest(hConnect, L"POST", requestPath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }

    BOOL sent = FALSE;
    if (hRequest) {
        // Gemini doesn't need Authorization header (key is in URL)
        WinHttpAddRequestHeaders(hRequest, L"Accept: text/event-stream", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    }

    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &statusSize, nullptr);
        if (status >= 200 && status < 300) {
            // ---- SSE read loop ----
            std::string buffer;
            char chunk[8192];
            DWORD read = 0;
            bool emitted = false;
            bool streamError = false;
            while (WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &read) && read > 0) {
                // Check cancel flag -- user pressed Stop
                if (g_nvCancelStream) {
                    g_nvCancelStream = false;
                    streamDone = true;
                    break;
                }
                chunk[read] = '\0';
                buffer += chunk;
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                        line.pop_back();
                    if (line.rfind("data:", 0) == 0) {
                        std::string data = line.substr(5);
                        size_t start = data.find_first_not_of(" \t");
                        if (start != std::string::npos) data = data.substr(start);
                        if (data == "[DONE]") { streamDone = true; break; }
                        if (!data.empty()) {
                            // Gemini delta extraction: candidates[0].content.parts[0].text
                            // Look for "text" field in the response
                            size_t textPos = data.find("\"text\"");
                            if (textPos != std::string::npos) {
                                size_t q1 = data.find('"', textPos + 6);
                                if (q1 != std::string::npos) {
                                    // walk to closing unescaped quote
                                    std::string raw;
                                    for (size_t k = q1 + 1; k < data.size(); k++) {
                                        if (data[k] == '\\' && k + 1 < data.size()) {
                                            char e = data[k + 1];
                                            if      (e == 'n')  { raw += '\n'; k++; }
                                            else if (e == 't')  { raw += '\t'; k++; }
                                            else if (e == 'r')  { raw += '\r'; k++; }
                                            else if (e == '"')  { raw += '"';  k++; }
                                            else if (e == '\\') { raw += '\\'; k++; }
                                            else if (e == '/')  { raw += '/';  k++; }
                                            else if (e == 'b')  { raw += '\b'; k++; }
                                            else if (e == 'f')  { raw += '\f'; k++; }
                                            else if (e == 'u' && k + 5 < data.size()) {
                                                // Decode \uXXXX hex unicode codepoint
                                                char hex[5] = { data[k+2], data[k+3], data[k+4], data[k+5], 0 };
                                                unsigned int cp = 0;
                                                for (int h = 0; h < 4; h++) {
                                                    cp <<= 4;
                                                    char hc = hex[h];
                                                    if (hc >= '0' && hc <= '9') cp |= (hc - '0');
                                                    else if (hc >= 'a' && hc <= 'f') cp |= (hc - 'a' + 10);
                                                    else if (hc >= 'A' && hc <= 'F') cp |= (hc - 'A' + 10);
                                                }
                                                // Encode as UTF-8
                                                if (cp < 0x80) {
                                                    raw += (char)cp;
                                                } else if (cp < 0x800) {
                                                    raw += (char)(0xC0 | (cp >> 6));
                                                    raw += (char)(0x80 | (cp & 0x3F));
                                                } else {
                                                    raw += (char)(0xE0 | (cp >> 12));
                                                    raw += (char)(0x80 | ((cp >> 6) & 0x3F));
                                                    raw += (char)(0x80 | (cp & 0x3F));
                                                }
                                                k += 5;
                                            }
                                            else { raw += e; k++; }
                                        } else if (data[k] == '"') break;
                                        else raw += data[k];
                                    }
                                    if (!raw.empty() && raw != "null") {
                                        emitted = true;
                                        onDelta(raw.c_str(), raw.size(), userCtx);
                                    }
                                }
                            }
                        }
                    }
                }
                if (streamDone) break;
            }
            if (streamDone || emitted) {
                if (hRequest) WinHttpCloseHandle(hRequest);
                if (hConnect) WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                SecureZeroMemory(const_cast<char*>(body.data()), body.size());
                return true;
            }
            (void)streamError;
        }
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    SecureZeroMemory(const_cast<char*>(body.data()), body.size());
    return false;
}

// ---------- key hygiene: zero a key buffer after use ----------
void ZeroApiKey(char* key, size_t len) {
    if (key && len) SecureZeroMemory(key, len);
}