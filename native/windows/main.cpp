#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <Shlwapi.h>
#include <wrl.h>
#include <WebView2.h>
#include "resource.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND window_handle = nullptr;
ComPtr<ICoreWebView2Environment> web_environment;
ComPtr<ICoreWebView2Controller> controller;
ComPtr<ICoreWebView2> webview;
ComPtr<ICoreWebView2_3> webview3;
std::filesystem::path executable_path;
std::filesystem::path executable_dir;
std::filesystem::path app_dir;
std::filesystem::path data_dir;
std::wstring app_name = L"Tiny";
std::wstring storage_mode = L"appData";
std::wstring data_dir_override;
std::wstring allowed_origin;
std::wstring start_url;
int window_width = 1200;
int window_height = 800;
std::wstring bundle_manifest;
std::vector<std::uint8_t> bundle_bytes;
std::uint64_t bundle_data_start = 0;
std::uint64_t bundle_data_end = 0;
bool development = false;
bool devtools = false;
bool bundled = false;
std::optional<COLORREF> titlebar_color;
std::optional<COLORREF> titlebar_text_color;
constexpr COLORREF startup_background = RGB(31, 31, 31);
HBRUSH startup_background_brush = nullptr;

COREWEBVIEW2_COLOR startup_webview_background() {
  return {
      255,
      GetRValue(startup_background),
      GetGValue(startup_background),
      GetBValue(startup_background)};
}

const auto startup_clock = std::chrono::steady_clock::now();

void startup_log(const wchar_t* stage) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startup_clock).count();
  OutputDebugStringW((L"[Tiny] " + std::wstring(stage) + L" +" + std::to_wstring(elapsed) + L"ms\n").c_str());
}

std::filesystem::path module_path() {
  std::wstring value(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length) return {};
    if (length < value.size() - 1) {
      value.resize(length);
      return std::filesystem::path(value);
    }
    value.resize(value.size() * 2);
  }
}

std::wstring lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), towlower);
  return value;
}

std::wstring safe_name(std::wstring value) {
  for (auto& character : value) {
    if (character == L'<' || character == L'>' || character == L':' || character == L'\"' ||
        character == L'/' || character == L'\\' || character == L'|' || character == L'?' || character == L'*') {
      character = L'-';
    }
  }
  while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) value.pop_back();
  return value.empty() ? L"Tiny" : value;
}

std::filesystem::path roaming_data_dir() {
  wchar_t value[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, value))) return {};
  return std::filesystem::path(value) / safe_name(app_name);
}

std::filesystem::path local_app_data_dir() {
  wchar_t value[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, value))) return {};
  return std::filesystem::path(value) / safe_name(app_name);
}

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!length) return {};
  std::wstring result(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!length) return {};
  std::string result(length, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
  return result;
}

std::optional<COLORREF> color_from_hex(const std::wstring& value) {
  if (value.size() != 7 || value[0] != L'#') return std::nullopt;
  unsigned int rgb = 0;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const wchar_t character = towlower(value[index]);
    if (character < L'0' || character > L'f' || (character > L'9' && character < L'a')) return std::nullopt;
    rgb = (rgb << 4) | (character <= L'9' ? character - L'0' : character - L'a' + 10);
  }
  return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

std::optional<std::wstring> json_raw_value(const std::wstring& json, const std::wstring& key, std::size_t from = 0) {
  const std::wstring needle = L"\"" + key + L"\"";
  const auto key_position = json.find(needle, from);
  if (key_position == std::wstring::npos) return std::nullopt;
  auto position = json.find(L':', key_position + needle.size());
  if (position == std::wstring::npos) return std::nullopt;
  while (++position < json.size() && iswspace(json[position])) {}
  if (position >= json.size()) return std::nullopt;
  const auto start = position;
  if (json[position] == L'\"') {
    for (++position; position < json.size(); ++position) {
      if (json[position] == L'\\') {
        ++position;
      } else if (json[position] == L'\"') {
        return json.substr(start, position - start + 1);
      }
    }
    return std::nullopt;
  }
  if (json[position] == L'{' || json[position] == L'[') {
    const wchar_t opening = json[position];
    const wchar_t closing = opening == L'{' ? L'}' : L']';
    int depth = 0;
    bool quoted = false;
    for (; position < json.size(); ++position) {
      if (json[position] == L'\\' && quoted) {
        ++position;
      } else if (json[position] == L'\"') {
        quoted = !quoted;
      } else if (!quoted && json[position] == opening) {
        ++depth;
      } else if (!quoted && json[position] == closing && --depth == 0) {
        return json.substr(start, position - start + 1);
      }
    }
    return std::nullopt;
  }
  while (position < json.size() && json[position] != L',' && json[position] != L'}') ++position;
  auto end = position;
  while (end > start && iswspace(json[end - 1])) --end;
  return end == start ? std::nullopt : std::optional<std::wstring>(json.substr(start, end - start));
}

std::optional<std::wstring> json_string(const std::wstring& json, const std::wstring& key, std::size_t from = 0) {
  const auto raw = json_raw_value(json, key, from);
  if (!raw || raw->size() < 2 || raw->front() != L'\"' || raw->back() != L'\"') return std::nullopt;
  std::wstring value;
  for (std::size_t index = 1; index + 1 < raw->size(); ++index) {
    if ((*raw)[index] != L'\\') {
      value += (*raw)[index];
      continue;
    }
    if (++index + 1 >= raw->size()) return std::nullopt;
    const wchar_t escaped = (*raw)[index];
    if (escaped == L'\"' || escaped == L'\\' || escaped == L'/') value += escaped;
    else if (escaped == L'b') value += L'\b';
    else if (escaped == L'f') value += L'\f';
    else if (escaped == L'n') value += L'\n';
    else if (escaped == L'r') value += L'\r';
    else if (escaped == L't') value += L'\t';
    else if (escaped == L'u' && index + 4 < raw->size()) {
      wchar_t code[5] = {(*raw)[index + 1], (*raw)[index + 2], (*raw)[index + 3], (*raw)[index + 4], L'\0'};
      value += static_cast<wchar_t>(wcstoul(code, nullptr, 16));
      index += 4;
    } else return std::nullopt;
  }
  return value;
}

std::optional<std::uint64_t> json_number(const std::wstring& json, const std::wstring& key, std::size_t from) {
  const auto raw = json_raw_value(json, key, from);
  if (!raw) return std::nullopt;
  try {
    return std::stoull(*raw);
  } catch (...) {
    return std::nullopt;
  }
}

std::wstring json_quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (const wchar_t character : value) {
    switch (character) {
      case L'\"': result += L"\\\""; break;
      case L'\\': result += L"\\\\"; break;
      case L'\b': result += L"\\b"; break;
      case L'\f': result += L"\\f"; break;
      case L'\n': result += L"\\n"; break;
      case L'\r': result += L"\\r"; break;
      case L'\t': result += L"\\t"; break;
      default: result += character;
    }
  }
  return result + L"\"";
}

void send(const std::wstring& value) {
  if (webview) webview->PostWebMessageAsString(value.c_str());
}

void result_raw(const std::wstring& id, const std::wstring& value) {
  send(L"{\"id\":" + json_quote(id) + L",\"result\":" + value + L"}");
}

void result_empty(const std::wstring& id) {
  result_raw(id, L"null");
}

void result_string(const std::wstring& id, const std::wstring& value) {
  result_raw(id, json_quote(value));
}

void result_error(const std::wstring& id, const std::wstring& code, const std::wstring& message) {
  send(L"{\"id\":" + json_quote(id) + L",\"error\":{\"code\":" + json_quote(code) + L",\"message\":" + json_quote(message) + L"}}");
}

bool valid_store(const std::wstring& store) {
  return !store.empty() && store.size() <= 64 && std::all_of(store.begin(), store.end(), [](wchar_t character) {
    return iswalnum(character) || character == L'_' || character == L'-';
  });
}

std::optional<std::filesystem::path> store_path(const std::wstring& id, const std::wstring& json) {
  const auto store = json_string(json, L"store");
  if (!store || !valid_store(*store)) {
    result_error(id, L"INVALID_STORE", L"Store names may contain only letters, numbers, hyphens, and underscores.");
    return std::nullopt;
  }
  return data_dir / (*store + L".json");
}

void write_store(const std::wstring& id, const std::filesystem::path& path, const std::wstring& value) {
  const auto bytes = wide_to_utf8(value);
  if (!value.empty() && bytes.empty()) return result_error(id, L"INVALID_JSON", L"The JSON value is not valid UTF-8.");
  // ponytail: one fixed temp file assumes one app writer; unique temp names can come later if needed.
  const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return result_error(id, L"WRITE_FAILED", L"The JSON store could not be opened.");
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return result_error(id, L"WRITE_FAILED", L"The JSON store could not be saved.");
  }
  result_empty(id);
}

void handle_message(const std::wstring& json) {
  const auto id = json_string(json, L"id");
  const auto method = json_string(json, L"method");
  if (!id || !method) return;

  if (*method == L"app.getDataPath") return result_string(*id, data_dir.wstring());
  if (*method == L"window.close") {
    PostMessageW(window_handle, WM_CLOSE, 0, 0);
    return result_empty(*id);
  }
  if (*method == L"window.minimize") {
    ShowWindow(window_handle, SW_MINIMIZE);
    return result_empty(*id);
  }
  if (*method == L"window.maximize") {
    ShowWindow(window_handle, SW_MAXIMIZE);
    return result_empty(*id);
  }
  if (*method == L"shell.openExternal") {
    const auto url = json_string(json, L"url");
    if (!url || (url->rfind(L"https://", 0) != 0 && url->rfind(L"http://", 0) != 0)) {
      return result_error(*id, L"INVALID_URL", L"Only HTTP and HTTPS URLs can be opened.");
    }
    const auto launched = reinterpret_cast<INT_PTR>(ShellExecuteW(window_handle, L"open", url->c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (launched <= 32) return result_error(*id, L"OPEN_FAILED", L"The system browser could not open the URL.");
    return result_empty(*id);
  }
  if (*method == L"data.read") {
    const auto path = store_path(*id, json);
    if (!path) return;
    std::ifstream input(*path, std::ios::binary);
    if (!input) return result_empty(*id);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto value = utf8_to_wide(contents);
    if (!contents.empty() && value.empty()) return result_error(*id, L"INVALID_JSON", L"The JSON store is not valid UTF-8.");
    return result_raw(*id, value.empty() ? L"null" : value);
  }
  if (*method == L"data.write") {
    const auto path = store_path(*id, json);
    const auto value = json_raw_value(json, L"value");
    if (!path || !value) {
      if (path) result_error(*id, L"INVALID_JSON", L"A JSON value is required.");
      return;
    }
    return write_store(*id, *path, *value);
  }

  result_error(*id, L"METHOD_NOT_FOUND", L"The native method does not exist.");
}

constexpr wchar_t bridge_script[] = LR"JS(
(() => {
  const pending = new Map();
  let nextId = 1;
  const request = (method, params = {}) => new Promise((resolve, reject) => {
    const id = String(nextId++);
    pending.set(id, { resolve, reject });
    window.chrome.webview.postMessage(JSON.stringify({ id, method, ...params }));
  });
  window.chrome.webview.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    const pendingRequest = pending.get(message.id);
    if (!pendingRequest) return;
    pending.delete(message.id);
    if (message.error) {
      const error = new Error(message.error.message);
      Object.assign(error, message.error);
      pendingRequest.reject(error);
    } else pendingRequest.resolve(message.result);
  });
  window.tiny = Object.freeze({
    app: { getDataPath: () => request('app.getDataPath') },
    data: {
      read: store => request('data.read', { store }),
      write: (store, value) => request('data.write', { store, value })
    },
    window: {
      close: () => request('window.close'),
      minimize: () => request('window.minimize'),
      maximize: () => request('window.maximize')
    },
    shell: { openExternal: url => request('shell.openExternal', { url }) }
  });
})();
)JS";

std::wstring origin_of(const std::wstring& url) {
  const auto scheme_end = url.find(L"://");
  if (scheme_end == std::wstring::npos) return {};
  const auto path_start = url.find(L'/', scheme_end + 3);
  return url.substr(0, path_start == std::wstring::npos ? url.size() : path_start);
}

bool same_origin(const std::wstring& url, const std::wstring& origin) {
  if (url.rfind(origin, 0) != 0) return false;
  return url.size() == origin.size() || url[origin.size()] == L'/' || url[origin.size()] == L'?' || url[origin.size()] == L'#';
}

bool read_u64(std::size_t offset, std::uint64_t* value) {
  if (offset + sizeof(std::uint64_t) > bundle_bytes.size()) return false;
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) result |= static_cast<std::uint64_t>(bundle_bytes[offset + index]) << (index * 8);
  *value = result;
  return true;
}

bool has_magic(std::size_t offset, const char* magic) {
  return offset + 8 <= bundle_bytes.size() && std::equal(magic, magic + 8, bundle_bytes.begin() + offset);
}

bool load_bundle() {
  std::ifstream input(executable_path, std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 16) return false;
  bundle_bytes.resize(static_cast<std::size_t>(size));
  input.seekg(0, std::ios::beg);
  input.read(reinterpret_cast<char*>(bundle_bytes.data()), static_cast<std::streamsize>(bundle_bytes.size()));
  const auto footer = bundle_bytes.size() - 16;
  std::uint64_t header = 0;
  std::uint64_t manifest_size = 0;
  if (!has_magic(footer, "TINYEND1") || !read_u64(footer + 8, &header) || !has_magic(static_cast<std::size_t>(header), "TINYBND1") ||
      !read_u64(static_cast<std::size_t>(header) + 8, &manifest_size)) return false;
  const auto manifest_start = static_cast<std::size_t>(header) + 16;
  if (manifest_start + manifest_size > footer) return false;
  const std::string manifest(reinterpret_cast<const char*>(bundle_bytes.data() + manifest_start), static_cast<std::size_t>(manifest_size));
  bundle_manifest = utf8_to_wide(manifest);
  if (!manifest.empty() && bundle_manifest.empty()) return false;
  bundle_data_start = manifest_start + manifest_size;
  bundle_data_end = footer;
  bundled = true;
  if (const auto value = json_string(bundle_manifest, L"appName")) app_name = *value;
  if (const auto value = json_string(bundle_manifest, L"storage")) storage_mode = *value;
  if (const auto value = json_number(bundle_manifest, L"windowWidth", 0)) window_width = static_cast<int>(*value);
  if (const auto value = json_number(bundle_manifest, L"windowHeight", 0)) window_height = static_cast<int>(*value);
  if (const auto value = json_string(bundle_manifest, L"titlebarColor")) titlebar_color = color_from_hex(*value);
  if (const auto value = json_string(bundle_manifest, L"titlebarTextColor")) titlebar_text_color = color_from_hex(*value);
  return true;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> bundle_range(const std::wstring& path) {
  const auto entry = bundle_manifest.find(L"\"path\":" + json_quote(path));
  if (entry == std::wstring::npos) return std::nullopt;
  const auto offset = json_number(bundle_manifest, L"offset", entry);
  const auto size = json_number(bundle_manifest, L"size", entry);
  if (!offset || !size || bundle_data_start + *offset + *size > bundle_data_end) return std::nullopt;
  return std::make_pair(bundle_data_start + *offset, *size);
}

std::wstring request_path(const std::wstring& uri) {
  const auto scheme_end = uri.find(L"://");
  auto slash = uri.find(L'/', scheme_end == std::wstring::npos ? 0 : scheme_end + 3);
  if (slash == std::wstring::npos || slash + 1 >= uri.size()) return L"index.html";
  auto path = uri.substr(slash + 1);
  const auto query = path.find_first_of(L"?#");
  if (query != std::wstring::npos) path.resize(query);
  return path.empty() ? L"index.html" : path;
}

std::wstring content_type(const std::wstring& path) {
  const auto value = lower(path);
  if (value.size() >= 5 && value.rfind(L".html") == value.size() - 5) return L"text/html; charset=utf-8";
  if (value.size() >= 3 && value.rfind(L".js") == value.size() - 3) return L"text/javascript; charset=utf-8";
  if (value.size() >= 4 && value.rfind(L".css") == value.size() - 4) return L"text/css; charset=utf-8";
  if (value.size() >= 5 && value.rfind(L".json") == value.size() - 5) return L"application/json; charset=utf-8";
  if (value.size() >= 4 && value.rfind(L".svg") == value.size() - 4) return L"image/svg+xml";
  return L"application/octet-stream";
}

void serve_bundle(ICoreWebView2WebResourceRequestedEventArgs* args) {
  ComPtr<ICoreWebView2WebResourceRequest> request;
  if (FAILED(args->get_Request(&request))) return;
  LPWSTR raw_uri = nullptr;
  request->get_Uri(&raw_uri);
  const std::wstring path = request_path(raw_uri ? raw_uri : L"");
  CoTaskMemFree(raw_uri);
  const auto range = bundle_range(path);
  const std::wstring headers = L"Content-Type: " + content_type(path) + L"\r\nCache-Control: no-cache";
  ComPtr<IStream> content;
  ComPtr<ICoreWebView2WebResourceResponse> response;
  if (range) content.Attach(SHCreateMemStream(bundle_bytes.data() + range->first, static_cast<UINT>(range->second)));
  if (FAILED(web_environment->CreateWebResourceResponse(content.Get(), range ? 200 : 404, range ? L"OK" : L"Not Found", headers.c_str(), &response))) return;
  args->put_Response(response.Get());
}

void navigate() {
  if (development) {
    webview->Navigate(start_url.c_str());
    if (devtools) webview->OpenDevToolsWindow();
    return;
  }
  if (bundled) {
    webview->Navigate(L"https://app.local/index.html");
    return;
  }
  if (FAILED(webview.As(&webview3)) || FAILED(webview3->SetVirtualHostNameToFolderMapping(
      L"app.local", app_dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW))) {
    MessageBoxW(window_handle, L"Could not map the application assets.", L"Tiny", MB_ICONERROR);
    PostQuitMessage(1);
    return;
  }
  webview->Navigate(L"https://app.local/index.html");
}

void configure_webview() {
  ComPtr<ICoreWebView2Settings> settings;
  webview->get_Settings(&settings);
  settings->put_AreDevToolsEnabled(devtools ? TRUE : FALSE);
  settings->put_IsStatusBarEnabled(FALSE);
  if (!development) settings->put_AreDefaultContextMenusEnabled(FALSE);

  EventRegistrationToken token{};
  webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        LPWSTR raw_uri = nullptr;
        args->get_Uri(&raw_uri);
        const std::wstring uri = raw_uri ? raw_uri : L"";
        CoTaskMemFree(raw_uri);
        if (!same_origin(uri, development ? allowed_origin : L"https://app.local")) args->put_Cancel(TRUE);
        else startup_log(L"navigation-starting");
        return S_OK;
      }).Get(), &token);
  webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE);
        return S_OK;
      }).Get(), &token);
  webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
        BOOL success = FALSE;
        args->get_IsSuccess(&success);
        startup_log(success ? L"navigation-completed" : L"navigation-failed");
        return S_OK;
      }).Get(), &token);
  webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        LPWSTR raw_message = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw_message))) {
          handle_message(raw_message ? raw_message : L"{}");
          CoTaskMemFree(raw_message);
        }
        return S_OK;
      }).Get(), &token);
  if (bundled) {
    webview->AddWebResourceRequestedFilter(L"https://app.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
          serve_bundle(args);
          return S_OK;
        }).Get(), &token);
  }
  webview->AddScriptToExecuteOnDocumentCreated(bridge_script,
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [](HRESULT result, LPCWSTR) -> HRESULT {
            if (FAILED(result)) {
              MessageBoxW(window_handle, L"Could not install the native bridge.", L"Tiny", MB_ICONERROR);
              PostQuitMessage(1);
            } else {
              startup_log(L"bridge-ready");
              navigate();
            }
            return S_OK;
          }).Get());
}

LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM w_param, LPARAM l_param) {
  if (message == WM_ERASEBKGND && startup_background_brush) {
    RECT bounds{};
    GetClientRect(handle, &bounds);
    FillRect(reinterpret_cast<HDC>(w_param), &bounds, startup_background_brush);
    return 1;
  }
  if (message == WM_SIZE && controller) {
    RECT bounds{};
    GetClientRect(handle, &bounds);
    controller->put_Bounds(bounds);
  } else if (message == WM_DESTROY) {
    PostQuitMessage(0);
  }
  return DefWindowProcW(handle, message, w_param, l_param);
}

void configure_titlebar() {
  constexpr DWORD dark_mode_attribute = 20;
  constexpr DWORD caption_color_attribute = 35;
  constexpr DWORD text_color_attribute = 36;
  BOOL dark = TRUE;
  DwmSetWindowAttribute(window_handle, dark_mode_attribute, &dark, sizeof(dark));
  if (titlebar_color) DwmSetWindowAttribute(window_handle, caption_color_attribute, &*titlebar_color, sizeof(COLORREF));
  if (titlebar_text_color) DwmSetWindowAttribute(window_handle, text_color_attribute, &*titlebar_text_color, sizeof(COLORREF));
}

void create_webview() {
  const auto local_data = local_app_data_dir();
  const auto user_data = storage_mode == L"portable" || local_data.empty()
      ? data_dir / L"WebView2"
      : local_data / L"WebView2";
  std::error_code error;
  std::filesystem::create_directories(user_data, error);
  if (error) {
    MessageBoxW(window_handle, L"Could not create the application data directory.", L"Tiny", MB_ICONERROR);
    PostQuitMessage(1);
    return;
  }
  startup_log(L"webview-environment-requested");
  CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              MessageBoxW(window_handle, L"WebView2 is not installed or could not start.", L"Tiny", MB_ICONERROR);
              PostQuitMessage(1);
              return result;
            }
            startup_log(L"webview-environment-ready");
            web_environment = environment;
            auto controller_handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT result, ICoreWebView2Controller* value) -> HRESULT {
                      if (FAILED(result) || !value) {
                        MessageBoxW(window_handle, L"Could not create the WebView2 window.", L"Tiny", MB_ICONERROR);
                        PostQuitMessage(1);
                        return result;
                      }
                      controller = value;
                      startup_log(L"webview-controller-ready");
                      ComPtr<ICoreWebView2Controller2> controller2;
                      if (SUCCEEDED(controller.As(&controller2))) controller2->put_DefaultBackgroundColor(startup_webview_background());
                      controller->get_CoreWebView2(&webview);
                      RECT bounds{};
                      GetClientRect(window_handle, &bounds);
                      controller->put_Bounds(bounds);
                      configure_webview();
                      return S_OK;
                    });
            ComPtr<ICoreWebView2Environment10> environment10;
            ComPtr<ICoreWebView2ControllerOptions> options;
            if (SUCCEEDED(environment->QueryInterface(IID_PPV_ARGS(&environment10))) &&
                SUCCEEDED(environment10->CreateCoreWebView2ControllerOptions(&options))) {
              ComPtr<ICoreWebView2ControllerOptions3> options3;
              if (SUCCEEDED(options.As(&options3))) options3->put_DefaultBackgroundColor(startup_webview_background());
              return environment10->CreateCoreWebView2ControllerWithOptions(
                  window_handle, options.Get(), controller_handler.Get());
            }
            return environment->CreateCoreWebView2Controller(window_handle, controller_handler.Get());
          }).Get());
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  for (int index = 1; index < argument_count; ++index) {
    const std::wstring argument = arguments[index];
    if (argument == L"--dev" && index + 1 < argument_count) {
      development = true;
      start_url = arguments[++index];
      allowed_origin = origin_of(start_url);
    } else if (argument == L"--app" && index + 1 < argument_count) {
      app_dir = std::filesystem::absolute(arguments[++index]);
    } else if (argument == L"--app-name" && index + 1 < argument_count) {
      app_name = arguments[++index];
    } else if (argument == L"--storage" && index + 1 < argument_count) {
      storage_mode = arguments[++index];
    } else if (argument == L"--data-dir" && index + 1 < argument_count) {
      data_dir_override = arguments[++index];
    } else if (argument == L"--window-width" && index + 1 < argument_count) {
      window_width = std::stoi(arguments[++index]);
    } else if (argument == L"--window-height" && index + 1 < argument_count) {
      window_height = std::stoi(arguments[++index]);
    } else if (argument == L"--titlebar-color" && index + 1 < argument_count) {
      titlebar_color = color_from_hex(arguments[++index]);
    } else if (argument == L"--titlebar-text-color" && index + 1 < argument_count) {
      titlebar_text_color = color_from_hex(arguments[++index]);
    } else if (argument == L"--devtools") {
      devtools = true;
    }
  }
  LocalFree(arguments);

  executable_path = module_path();
  executable_dir = executable_path.parent_path();
  if (!development) load_bundle();
  app_name = safe_name(app_name);
  if (storage_mode != L"portable") storage_mode = L"appData";
  if (app_dir.empty()) app_dir = executable_dir / L"app";
  if (!development && !bundled && !std::filesystem::exists(app_dir / L"index.html")) {
    MessageBoxW(nullptr, L"The application assets are missing.", L"Tiny", MB_ICONERROR);
    return 1;
  }
  data_dir = data_dir_override.empty()
      ? (storage_mode == L"portable" ? executable_dir / L"data" : roaming_data_dir())
      : std::filesystem::absolute(data_dir_override);
  if (data_dir.empty()) return 1;
  std::error_code error;
  std::filesystem::create_directories(data_dir, error);
  if (error) {
    MessageBoxW(nullptr, L"Could not create the application data directory.", L"Tiny", MB_ICONERROR);
    return 1;
  }

  WNDCLASSW window_class{};
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_TINY_ICON));
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = L"TinyWindow";
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  startup_background_brush = CreateSolidBrush(startup_background);
  window_class.hbrBackground = startup_background_brush;
  RegisterClassW(&window_class);

  window_handle = CreateWindowW(window_class.lpszClassName, app_name.c_str(), WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, window_width, window_height, nullptr, nullptr, instance, nullptr);
  if (!window_handle) return 1;
  configure_titlebar();
  ShowWindow(window_handle, SW_SHOW);
  startup_log(L"window-shown");

  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 1;
  create_webview();

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  CoUninitialize();
  return static_cast<int>(message.wParam);
}
