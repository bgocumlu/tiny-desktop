#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <limits.h>
#include <unistd.h>

namespace {

GtkWidget* window_handle = nullptr;
WebKitWebView* webview = nullptr;
std::filesystem::path executable_path;
std::filesystem::path executable_dir;
std::filesystem::path app_dir;
std::filesystem::path data_dir;
std::string app_name = "Tiny";
std::string storage_mode = "appData";
std::string data_dir_override;
std::string allowed_origin;
std::string start_url;
int window_width = 1200;
int window_height = 800;
std::string bundle_manifest;
std::vector<std::uint8_t> bundle_bytes;
std::uint64_t bundle_data_start = 0;
std::uint64_t bundle_data_end = 0;
bool development = false;
bool devtools = false;
bool bundled = false;

const auto startup_clock = std::chrono::steady_clock::now();

void startup_log(const char* stage) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startup_clock).count();
  std::cerr << "[Tiny] " << stage << " +" << elapsed << "ms\n";
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string safe_name(std::string value) {
  for (auto& character : value) {
    if (character == '<' || character == '>' || character == ':' || character == '"' ||
        character == '/' || character == '\\' || character == '|' || character == '?' || character == '*') {
      character = '-';
    }
  }
  while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
  return value.empty() ? "Tiny" : value;
}

std::filesystem::path module_path() {
  std::vector<char> value(256);
  for (;;) {
    const auto length = readlink("/proc/self/exe", value.data(), value.size() - 1);
    if (length < 0) return {};
    if (static_cast<std::size_t>(length) < value.size() - 1) {
      return std::filesystem::path(std::string(value.data(), static_cast<std::size_t>(length)));
    }
    value.resize(value.size() * 2);
  }
}

std::string origin_of(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return {};
  const auto path_start = url.find('/', scheme_end + 3);
  return url.substr(0, path_start == std::string::npos ? url.size() : path_start);
}

bool same_origin(const std::string& url, const std::string& origin) {
  if (origin.empty() || url.rfind(origin, 0) != 0) return false;
  return url.size() == origin.size() || url[origin.size()] == '/' || url[origin.size()] == '?' || url[origin.size()] == '#';
}

int hex_digit(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

void append_utf8(std::string& result, std::uint32_t codepoint) {
  if (codepoint <= 0x7f) result.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7ff) {
    result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0x10ffff) {
    result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

std::optional<std::string> json_raw_value(const std::string& json, const std::string& key, std::size_t from = 0) {
  const std::string needle = "\"" + key + "\"";
  const auto key_position = json.find(needle, from);
  if (key_position == std::string::npos) return std::nullopt;
  auto position = json.find(':', key_position + needle.size());
  if (position == std::string::npos) return std::nullopt;
  while (++position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) {}
  if (position >= json.size()) return std::nullopt;
  const auto start = position;
  if (json[position] == '"') {
    for (++position; position < json.size(); ++position) {
      if (json[position] == '\\') ++position;
      else if (json[position] == '"') return json.substr(start, position - start + 1);
    }
    return std::nullopt;
  }
  if (json[position] == '{' || json[position] == '[') {
    const char opening = json[position];
    const char closing = opening == '{' ? '}' : ']';
    int depth = 0;
    bool quoted = false;
    for (; position < json.size(); ++position) {
      if (json[position] == '\\' && quoted) ++position;
      else if (json[position] == '"') quoted = !quoted;
      else if (!quoted && json[position] == opening) ++depth;
      else if (!quoted && json[position] == closing && --depth == 0) return json.substr(start, position - start + 1);
    }
    return std::nullopt;
  }
  while (position < json.size() && json[position] != ',' && json[position] != '}') ++position;
  auto end = position;
  while (end > start && std::isspace(static_cast<unsigned char>(json[end - 1]))) --end;
  return end == start ? std::nullopt : std::optional<std::string>(json.substr(start, end - start));
}

std::optional<std::string> json_string(const std::string& json, const std::string& key, std::size_t from = 0) {
  const auto raw = json_raw_value(json, key, from);
  if (!raw || raw->size() < 2 || raw->front() != '"' || raw->back() != '"') return std::nullopt;
  std::string value;
  for (std::size_t index = 1; index + 1 < raw->size(); ++index) {
    if ((*raw)[index] != '\\') {
      value += (*raw)[index];
      continue;
    }
    if (++index + 1 >= raw->size()) return std::nullopt;
    const char escaped = (*raw)[index];
    if (escaped == '"' || escaped == '\\' || escaped == '/') value += escaped;
    else if (escaped == 'b') value += '\b';
    else if (escaped == 'f') value += '\f';
    else if (escaped == 'n') value += '\n';
    else if (escaped == 'r') value += '\r';
    else if (escaped == 't') value += '\t';
    else if (escaped == 'u' && index + 4 < raw->size()) {
      std::uint32_t codepoint = 0;
      for (std::size_t offset = 1; offset <= 4; ++offset) {
        const int digit = hex_digit((*raw)[index + offset]);
        if (digit < 0) return std::nullopt;
        codepoint = (codepoint << 4) | static_cast<std::uint32_t>(digit);
      }
      append_utf8(value, codepoint);
      index += 4;
    } else return std::nullopt;
  }
  return value;
}

std::optional<std::uint64_t> json_number(const std::string& json, const std::string& key, std::size_t from) {
  const auto raw = json_raw_value(json, key, from);
  if (!raw) return std::nullopt;
  try {
    return std::stoull(*raw);
  } catch (...) {
    return std::nullopt;
  }
}

std::string json_quote(const std::string& value) {
  std::string result = "\"";
  for (const unsigned char character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          char escaped[7]{};
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
          result += escaped;
        } else result += static_cast<char>(character);
    }
  }
  return result + "\"";
}

void send(const std::string& value) {
  if (!webview) return;
  const auto script = "window.__tinyDeliver(JSON.parse(" + json_quote(value) + "));";
  webkit_web_view_evaluate_javascript(webview, script.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void result_raw(const std::string& id, const std::string& value) {
  send("{\"id\":" + json_quote(id) + ",\"result\":" + value + "}");
}

void result_empty(const std::string& id) {
  result_raw(id, "null");
}

void result_string(const std::string& id, const std::string& value) {
  result_raw(id, json_quote(value));
}

void result_error(const std::string& id, const std::string& code, const std::string& message) {
  send("{\"id\":" + json_quote(id) + ",\"error\":{\"code\":" + json_quote(code) + ",\"message\":" + json_quote(message) + "}}");
}

bool valid_store(const std::string& store) {
  return !store.empty() && store.size() <= 64 && std::all_of(store.begin(), store.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '_' || character == '-';
  });
}

std::optional<std::filesystem::path> store_path(const std::string& id, const std::string& json) {
  const auto store = json_string(json, "store");
  if (!store || !valid_store(*store)) {
    result_error(id, "INVALID_STORE", "Store names may contain only letters, numbers, hyphens, and underscores.");
    return std::nullopt;
  }
  return data_dir / (*store + ".json");
}

void write_store(const std::string& id, const std::filesystem::path& path, const std::string& value) {
  if (!g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr)) {
    return result_error(id, "INVALID_JSON", "The JSON value is not valid UTF-8.");
  }
  // ponytail: one fixed temp file assumes one app writer; unique temp names can come later if needed.
  const auto temporary = std::filesystem::path(path.string() + ".tmp");
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return result_error(id, "WRITE_FAILED", "The JSON store could not be opened.");
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  output.close();
  if (!output || std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return result_error(id, "WRITE_FAILED", "The JSON store could not be saved.");
  }
  result_empty(id);
}

void handle_message(const std::string& json) {
  const auto id = json_string(json, "id");
  const auto method = json_string(json, "method");
  if (!id || !method) return;

  if (*method == "app.getDataPath") return result_string(*id, data_dir.string());
  if (*method == "window.close") {
    gtk_window_close(GTK_WINDOW(window_handle));
    return result_empty(*id);
  }
  if (*method == "window.minimize") {
    gtk_window_iconify(GTK_WINDOW(window_handle));
    return result_empty(*id);
  }
  if (*method == "window.maximize") {
    gtk_window_maximize(GTK_WINDOW(window_handle));
    return result_empty(*id);
  }
  if (*method == "shell.openExternal") {
    const auto url = json_string(json, "url");
    if (!url || (url->rfind("https://", 0) != 0 && url->rfind("http://", 0) != 0)) {
      return result_error(*id, "INVALID_URL", "Only HTTP and HTTPS URLs can be opened.");
    }
    GError* error = nullptr;
    const auto launched = g_app_info_launch_default_for_uri(url->c_str(), nullptr, &error);
    if (!launched) {
      if (error) g_error_free(error);
      return result_error(*id, "OPEN_FAILED", "The system browser could not open the URL.");
    }
    return result_empty(*id);
  }
  if (*method == "data.read") {
    const auto path = store_path(*id, json);
    if (!path) return;
    std::ifstream input(*path, std::ios::binary);
    if (!input) return result_empty(*id);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!g_utf8_validate(contents.data(), static_cast<gssize>(contents.size()), nullptr)) {
      return result_error(*id, "INVALID_JSON", "The JSON store is not valid UTF-8.");
    }
    return result_raw(*id, contents.empty() ? "null" : contents);
  }
  if (*method == "data.write") {
    const auto path = store_path(*id, json);
    const auto value = json_raw_value(json, "value");
    if (!path || !value) {
      if (path) result_error(*id, "INVALID_JSON", "A JSON value is required.");
      return;
    }
    return write_store(*id, *path, *value);
  }

  result_error(*id, "METHOD_NOT_FOUND", "The native method does not exist.");
}

constexpr char bridge_script[] = R"JS(
(() => {
  const pending = new Map();
  let nextId = 1;
  const request = (method, params = {}) => new Promise((resolve, reject) => {
    const id = String(nextId++);
    pending.set(id, { resolve, reject });
    window.webkit.messageHandlers.tiny.postMessage(JSON.stringify({ id, method, ...params }));
  });
  window.__tinyDeliver = message => {
    const pendingRequest = pending.get(message.id);
    if (!pendingRequest) return;
    pending.delete(message.id);
    if (message.error) {
      const error = new Error(message.error.message);
      Object.assign(error, message.error);
      pendingRequest.reject(error);
    } else pendingRequest.resolve(message.result);
  };
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

bool read_u64(std::size_t offset, std::uint64_t* value) {
  if (offset > bundle_bytes.size() || bundle_bytes.size() - offset < sizeof(std::uint64_t)) return false;
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    result |= static_cast<std::uint64_t>(bundle_bytes[offset + index]) << (index * 8);
  }
  *value = result;
  return true;
}

bool has_magic(std::size_t offset, const char* magic) {
  return offset <= bundle_bytes.size() && bundle_bytes.size() - offset >= 8 &&
      std::equal(magic, magic + 8, bundle_bytes.begin() + offset);
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
  if (!input) return false;

  const auto footer = bundle_bytes.size() - 16;
  std::uint64_t header = 0;
  std::uint64_t manifest_size = 0;
  if (!has_magic(footer, "TINYEND1") || !read_u64(footer + 8, &header) ||
      header > footer || !has_magic(static_cast<std::size_t>(header), "TINYBND1") ||
      !read_u64(static_cast<std::size_t>(header) + 8, &manifest_size)) return false;
  const auto manifest_start = static_cast<std::size_t>(header) + 16;
  if (manifest_start > footer || manifest_size > footer - manifest_start) return false;
  bundle_manifest.assign(reinterpret_cast<const char*>(bundle_bytes.data() + manifest_start), static_cast<std::size_t>(manifest_size));
  if (!g_utf8_validate(bundle_manifest.data(), static_cast<gssize>(bundle_manifest.size()), nullptr)) return false;
  bundle_data_start = manifest_start + manifest_size;
  bundle_data_end = footer;
  bundled = true;
  if (const auto value = json_string(bundle_manifest, "appName")) app_name = *value;
  if (const auto value = json_string(bundle_manifest, "storage")) storage_mode = *value;
  if (const auto value = json_number(bundle_manifest, "windowWidth", 0)) window_width = static_cast<int>(*value);
  if (const auto value = json_number(bundle_manifest, "windowHeight", 0)) window_height = static_cast<int>(*value);
  return true;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> bundle_range(const std::string& path) {
  const auto entry = bundle_manifest.find("\"path\":" + json_quote(path));
  if (entry == std::string::npos) return std::nullopt;
  const auto offset = json_number(bundle_manifest, "offset", entry);
  const auto size = json_number(bundle_manifest, "size", entry);
  if (!offset || !size || *offset > bundle_data_end - bundle_data_start ||
      *size > bundle_data_end - bundle_data_start - *offset) return std::nullopt;
  return std::make_pair(bundle_data_start + *offset, *size);
}

std::string request_path(const char* raw_path) {
  std::string path = raw_path ? raw_path : "";
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (const auto query = path.find_first_of("?#"); query != std::string::npos) path.resize(query);
  gchar* decoded = g_uri_unescape_string(path.c_str(), nullptr);
  if (decoded) {
    path = decoded;
    g_free(decoded);
  }
  return path.empty() ? "index.html" : path;
}

std::string content_type(const std::string& path) {
  const auto value = lower(path);
  if (ends_with(value, ".html")) return "text/html; charset=utf-8";
  if (ends_with(value, ".js")) return "text/javascript; charset=utf-8";
  if (ends_with(value, ".css")) return "text/css; charset=utf-8";
  if (ends_with(value, ".json")) return "application/json; charset=utf-8";
  if (ends_with(value, ".svg")) return "image/svg+xml";
  if (ends_with(value, ".png")) return "image/png";
  if (ends_with(value, ".jpg") || ends_with(value, ".jpeg")) return "image/jpeg";
  if (ends_with(value, ".gif")) return "image/gif";
  if (ends_with(value, ".webp")) return "image/webp";
  if (ends_with(value, ".woff2")) return "font/woff2";
  return "application/octet-stream";
}

void serve_bundle(WebKitURISchemeRequest* request, gpointer) {
  const auto path = request_path(webkit_uri_scheme_request_get_path(request));
  const auto range = bundle_range(path);
  if (!range) {
    GError* error = g_error_new_literal(g_quark_from_static_string("tiny"), 404, "Resource not found.");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }
  auto* stream = g_memory_input_stream_new_from_data(
      bundle_bytes.data() + range->first, static_cast<gssize>(range->second), nullptr);
  const auto mime = content_type(path);
  webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), static_cast<gint64>(range->second), mime.c_str());
  g_object_unref(stream);
}

void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
  auto* value = webkit_javascript_result_get_js_value(result);
  gchar* message = jsc_value_to_string(value);
  handle_message(message ? message : "{}");
  g_free(message);
  webkit_javascript_result_unref(result);
}

gboolean on_decide_policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
  auto* navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  auto* action = webkit_navigation_policy_decision_get_navigation_action(navigation);
  auto* request = webkit_navigation_action_get_request(action);
  const auto* uri = webkit_uri_request_get_uri(request);
  if (!uri || !same_origin(uri, development ? allowed_origin : "tiny://app")) {
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }
  return FALSE;
}

WebKitWebView* on_create_webview(WebKitWebView*, WebKitNavigationAction*, gpointer) {
  return nullptr;
}

void on_load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer) {
  if (event == WEBKIT_LOAD_STARTED) startup_log("navigation-starting");
  if (event == WEBKIT_LOAD_FINISHED) {
    startup_log("navigation-completed");
    if (devtools) webkit_web_inspector_show(webkit_web_view_get_inspector(webview));
  }
}

void on_load_failed(WebKitWebView*, WebKitLoadEvent, const gchar* failing_uri, GError* error, gpointer) {
  std::cerr << "[Tiny] navigation-failed " << (failing_uri ? failing_uri : "") << ": "
            << (error ? error->message : "unknown error") << "\n";
}

bool create_webview() {
  const auto webkit_data = data_dir / "WebKit";
  const auto webkit_cache = data_dir / "WebKitCache";
  std::error_code error;
  std::filesystem::create_directories(webkit_data, error);
  std::filesystem::create_directories(webkit_cache, error);
  if (error) {
    std::cerr << "Tiny: could not create the application data directory.\n";
    return false;
  }

  auto* website_data = webkit_website_data_manager_new(
      "base-data-directory", webkit_data.c_str(),
      "base-cache-directory", webkit_cache.c_str(), nullptr);
  auto* context = webkit_web_context_new_with_website_data_manager(website_data);
  webkit_web_context_register_uri_scheme(context, "tiny", serve_bundle, nullptr, nullptr);

  auto* manager = webkit_user_content_manager_new();
  g_signal_connect(manager, "script-message-received::tiny", G_CALLBACK(on_script_message), nullptr);
  if (!webkit_user_content_manager_register_script_message_handler(manager, "tiny")) {
    std::cerr << "Tiny: could not register the native bridge.\n";
    g_object_unref(manager);
    g_object_unref(context);
    g_object_unref(website_data);
    return false;
  }
  auto* script = webkit_user_script_new(
      bridge_script,
      WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
      nullptr,
      nullptr);
  webkit_user_content_manager_add_script(manager, script);
  webkit_user_script_unref(script);

  webview = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW,
      "web-context", context,
      "user-content-manager", manager,
      nullptr));
  g_object_unref(manager);
  g_object_unref(context);
  g_object_unref(website_data);

  auto* settings = webkit_web_view_get_settings(webview);
  webkit_settings_set_enable_developer_extras(settings, devtools);
  g_signal_connect(webview, "decide-policy", G_CALLBACK(on_decide_policy), nullptr);
  g_signal_connect(webview, "create", G_CALLBACK(on_create_webview), nullptr);
  g_signal_connect(webview, "load-changed", G_CALLBACK(on_load_changed), nullptr);
  g_signal_connect(webview, "load-failed", G_CALLBACK(on_load_failed), nullptr);
  gtk_container_add(GTK_CONTAINER(window_handle), GTK_WIDGET(webview));

  if (development) {
    webkit_web_view_load_uri(webview, start_url.c_str());
  } else if (bundled) {
    webkit_web_view_load_uri(webview, "tiny://app/index.html");
  } else {
    std::cerr << "Tiny: application assets are missing.\n";
    return false;
  }
  return true;
}

void parse_arguments(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dev" && index + 1 < argc) {
      development = true;
      start_url = argv[++index];
      allowed_origin = origin_of(start_url);
    } else if (argument == "--app" && index + 1 < argc) {
      app_dir = std::filesystem::absolute(argv[++index]);
    } else if (argument == "--app-name" && index + 1 < argc) {
      app_name = argv[++index];
    } else if (argument == "--storage" && index + 1 < argc) {
      storage_mode = argv[++index];
    } else if (argument == "--data-dir" && index + 1 < argc) {
      data_dir_override = argv[++index];
    } else if (argument == "--window-width" && index + 1 < argc) {
      window_width = std::stoi(argv[++index]);
    } else if (argument == "--window-height" && index + 1 < argc) {
      window_height = std::stoi(argv[++index]);
    } else if ((argument == "--titlebar-color" || argument == "--titlebar-text-color") && index + 1 < argc) {
      ++index;
    } else if (argument == "--devtools") {
      devtools = true;
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  parse_arguments(argc, argv);
  executable_path = module_path();
  executable_dir = executable_path.parent_path();
  if (!development && !load_bundle()) {
    std::cerr << "Tiny: application bundle is missing or invalid.\n";
    return 1;
  }
  app_name = safe_name(app_name);
  if (storage_mode != "portable") storage_mode = "appData";
  if (app_dir.empty()) app_dir = executable_dir / "app";
  data_dir = data_dir_override.empty()
      ? (storage_mode == "portable"
          ? executable_dir / "data"
          : std::filesystem::path(g_get_user_data_dir()) / safe_name(app_name))
      : std::filesystem::absolute(data_dir_override);
  if (data_dir.empty()) return 1;

  std::error_code error;
  std::filesystem::create_directories(data_dir, error);
  if (error) {
    std::cerr << "Tiny: could not create the application data directory.\n";
    return 1;
  }

  std::array<char*, 1> gtk_argv{argv[0]};
  int gtk_argc = 1;
  char** gtk_argv_data = gtk_argv.data();
  if (!gtk_init_check(&gtk_argc, &gtk_argv_data)) {
    std::cerr << "Tiny: could not initialize GTK.\n";
    return 1;
  }

  window_handle = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window_handle), app_name.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window_handle), window_width, window_height);
  g_signal_connect(window_handle, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  if (!create_webview()) {
    gtk_widget_destroy(window_handle);
    return 1;
  }
  gtk_widget_show_all(window_handle);
  startup_log("window-shown");
  gtk_main();
  return 0;
}
