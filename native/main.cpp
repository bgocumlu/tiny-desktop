#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND window_handle = nullptr;
ComPtr<ICoreWebView2Controller> controller;
ComPtr<ICoreWebView2> webview;
ComPtr<ICoreWebView2_3> webview3;
std::filesystem::path executable_dir;
std::filesystem::path app_dir;
std::filesystem::path data_dir;
std::wstring allowed_origin;
std::wstring start_url;
bool development = false;
bool devtools = false;

std::filesystem::path module_dir() {
  std::wstring value(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length) return {};
    if (length < value.size() - 1) {
      value.resize(length);
      return std::filesystem::path(value).parent_path();
    }
    value.resize(value.size() * 2);
  }
}

std::filesystem::path roaming_data_dir() {
  wchar_t value[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, value))) return {};
  return std::filesystem::path(value) / L"Tiny";
}

std::wstring lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), towlower);
  return value;
}

bool is_data_path(const std::filesystem::path& value) {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(data_dir, error);
  if (error) return false;
  const auto candidate = std::filesystem::weakly_canonical(value.is_relative() ? data_dir / value : value, error);
  if (error) return false;
  auto root_text = lower(root.wstring());
  auto candidate_text = lower(candidate.wstring());
  const auto root_prefix = root_text.back() == L'\\' ? root_text : root_text + L'\\';
  return candidate_text == root_text || candidate_text.rfind(root_prefix, 0) == 0;
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

std::optional<std::wstring> json_string(const std::wstring& json, const std::wstring& key) {
  const std::wstring needle = L"\"" + key + L"\"";
  const auto key_position = json.find(needle);
  if (key_position == std::wstring::npos) return std::nullopt;
  auto position = json.find(L':', key_position + needle.size());
  if (position == std::wstring::npos) return std::nullopt;
  while (++position < json.size() && iswspace(json[position])) {}
  if (position >= json.size() || json[position] != L'\"') return std::nullopt;
  std::wstring value;
  for (++position; position < json.size(); ++position) {
    const wchar_t character = json[position];
    if (character == L'\"') return value;
    if (character != L'\\') {
      value += character;
      continue;
    }
    if (++position >= json.size()) return std::nullopt;
    const wchar_t escaped = json[position];
    if (escaped == L'\"' || escaped == L'\\' || escaped == L'/') value += escaped;
    else if (escaped == L'b') value += L'\b';
    else if (escaped == L'f') value += L'\f';
    else if (escaped == L'n') value += L'\n';
    else if (escaped == L'r') value += L'\r';
    else if (escaped == L't') value += L'\t';
    else if (escaped == L'u' && position + 4 < json.size()) {
      wchar_t code[5] = {json[position + 1], json[position + 2], json[position + 3], json[position + 4], L'\0'};
      value += static_cast<wchar_t>(wcstoul(code, nullptr, 16));
      position += 4;
    } else return std::nullopt;
  }
  return std::nullopt;
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
      default:
        if (character < 0x20) {
          wchar_t escaped[7]{};
          swprintf_s(escaped, _countof(escaped), L"\\u%04x", static_cast<unsigned>(character));
          result += escaped;
        } else result += character;
    }
  }
  return result + L"\"";
}

void send(const std::wstring& value) {
  if (webview) webview->PostWebMessageAsString(value.c_str());
}

void result_string(const std::wstring& id, const std::wstring& value) {
  send(L"{\"id\":" + json_quote(id) + L",\"result\":" + json_quote(value) + L"}");
}

void result_bool(const std::wstring& id, bool value) {
  send(L"{\"id\":" + json_quote(id) + L",\"result\":" + (value ? L"true" : L"false") + L"}");
}

void result_empty(const std::wstring& id) {
  send(L"{\"id\":" + json_quote(id) + L",\"result\":null}");
}

void result_error(const std::wstring& id, const std::wstring& code, const std::wstring& message) {
  send(L"{\"id\":" + json_quote(id) + L",\"error\":{\"code\":" + json_quote(code) + L",\"message\":" + json_quote(message) + L"}}");
}

std::optional<std::filesystem::path> requested_path(const std::wstring& id, const std::wstring& json) {
  const auto value = json_string(json, L"path");
  if (!value || value->empty()) {
    result_error(id, L"INVALID_PATH", L"A path is required.");
    return std::nullopt;
  }
  const auto path = std::filesystem::path(*value);
  if (!is_data_path(path)) {
    result_error(id, L"PATH_DENIED", L"The path must stay inside the application data directory.");
    return std::nullopt;
  }
  return path;
}

void handle_message(const std::wstring& json) {
  const auto id = json_string(json, L"id");
  const auto method = json_string(json, L"method");
  if (!id || !method) return;

  if (*method == L"app.getDataPath") return result_string(*id, data_dir.wstring());
  if (*method == L"app.getExecutablePath") return result_string(*id, executable_dir.wstring());

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

  if (*method == L"fs.exists") {
    const auto path = requested_path(*id, json);
    if (!path) return;
    return result_bool(*id, std::filesystem::exists(*path));
  }
  if (*method == L"fs.mkdir") {
    const auto path = requested_path(*id, json);
    if (!path) return;
    std::error_code error;
    std::filesystem::create_directories(*path, error);
    if (error) return result_error(*id, L"MKDIR_FAILED", L"The directory could not be created.");
    return result_empty(*id);
  }
  if (*method == L"fs.readText") {
    const auto path = requested_path(*id, json);
    if (!path) return;
    std::ifstream input(*path, std::ios::binary);
    if (!input) return result_error(*id, L"FILE_NOT_FOUND", L"The requested file does not exist.");
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto text = utf8_to_wide(contents);
    if (!contents.empty() && text.empty()) return result_error(*id, L"INVALID_UTF8", L"The file is not valid UTF-8 text.");
    return result_string(*id, text);
  }
  if (*method == L"fs.writeText") {
    const auto path = requested_path(*id, json);
    const auto content = json_string(json, L"content");
    if (!path || !content) {
      if (path) result_error(*id, L"INVALID_CONTENT", L"Text content is required.");
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(path->parent_path(), error);
    if (error) return result_error(*id, L"MKDIR_FAILED", L"The parent directory could not be created.");
    // ponytail: one fixed temp file assumes one app writer; use unique temp names if concurrent writers matter.
    const auto temporary = std::filesystem::path(path->wstring() + L".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const auto bytes = wide_to_utf8(*content);
    if (!output || (!content->empty() && bytes.empty())) return result_error(*id, L"WRITE_FAILED", L"The file could not be written.");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return result_error(*id, L"WRITE_FAILED", L"The file could not be written.");
    if (!MoveFileExW(temporary.c_str(), path->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temporary.c_str());
      return result_error(*id, L"RENAME_FAILED", L"The temporary file could not be moved into place.");
    }
    return result_empty(*id);
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
  window.native = Object.freeze({
    app: {
      getDataPath: () => request('app.getDataPath'),
      getExecutablePath: () => request('app.getExecutablePath')
    },
    fs: {
      exists: path => request('fs.exists', { path }),
      mkdir: path => request('fs.mkdir', { path }),
      readText: path => request('fs.readText', { path }),
      writeText: (path, content) => request('fs.writeText', { path, content })
    },
    window: {
      close: () => request('window.close'),
      minimize: () => request('window.minimize'),
      maximize: () => request('window.maximize')
    },
    shell: {
      openExternal: url => request('shell.openExternal', { url })
    }
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

void navigate() {
  if (development) {
    webview->Navigate(start_url.c_str());
    if (devtools) webview->OpenDevToolsWindow();
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
        const bool allowed = same_origin(uri, development ? allowed_origin : L"https://app.local");
        if (!allowed) args->put_Cancel(TRUE);
        return S_OK;
      }).Get(), &token);
  webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
      [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
        args->put_Handled(TRUE);
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
  webview->AddScriptToExecuteOnDocumentCreated(bridge_script,
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [](HRESULT result, LPCWSTR) -> HRESULT {
            if (FAILED(result)) {
              MessageBoxW(window_handle, L"Could not install the native bridge.", L"Tiny", MB_ICONERROR);
              PostQuitMessage(1);
            } else navigate();
            return S_OK;
          }).Get());
}

LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM w_param, LPARAM l_param) {
  if (message == WM_SIZE && controller) {
    RECT bounds{};
    GetClientRect(handle, &bounds);
    controller->put_Bounds(bounds);
  } else if (message == WM_DESTROY) {
    PostQuitMessage(0);
  }
  return DefWindowProcW(handle, message, w_param, l_param);
}

void create_webview() {
  const auto user_data = data_dir / L"WebView2";
  std::error_code error;
  std::filesystem::create_directories(user_data, error);
  if (error) {
    MessageBoxW(window_handle, L"Could not create the application data directory.", L"Tiny", MB_ICONERROR);
    PostQuitMessage(1);
    return;
  }
  CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              MessageBoxW(window_handle, L"WebView2 is not installed or could not start.", L"Tiny", MB_ICONERROR);
              PostQuitMessage(1);
              return result;
            }
            return environment->CreateCoreWebView2Controller(window_handle,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT result, ICoreWebView2Controller* value) -> HRESULT {
                      if (FAILED(result) || !value) {
                        MessageBoxW(window_handle, L"Could not create the WebView2 window.", L"Tiny", MB_ICONERROR);
                        PostQuitMessage(1);
                        return result;
                      }
                      controller = value;
                      controller->get_CoreWebView2(&webview);
                      RECT bounds{};
                      GetClientRect(window_handle, &bounds);
                      controller->put_Bounds(bounds);
                      configure_webview();
                      return S_OK;
                    }).Get());
          }).Get());
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
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
    } else if (argument == L"--devtools") {
      devtools = true;
    }
  }
  LocalFree(arguments);

  executable_dir = module_dir();
  if (app_dir.empty()) app_dir = executable_dir / L"app";
  if (!development && !std::filesystem::exists(app_dir / L"index.html")) {
    MessageBoxW(nullptr, L"The application assets are missing.", L"Tiny", MB_ICONERROR);
    return 1;
  }
  data_dir = roaming_data_dir();
  if (data_dir.empty()) return 1;

  WNDCLASSW window_class{};
  window_class.hInstance = instance;
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = L"TinyWindow";
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&window_class);

  window_handle = CreateWindowW(window_class.lpszClassName, L"Tiny", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, instance, nullptr);
  if (!window_handle) return 1;
  ShowWindow(window_handle, show_command);

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
