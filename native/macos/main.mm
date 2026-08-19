#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <mach-o/dyld.h>

namespace {

NSWindow* window_handle = nil;
WKWebView* webview = nil;
std::filesystem::path executable_path;
std::filesystem::path executable_dir;
std::filesystem::path data_dir;
std::string app_name = "Tiny";
std::string storage_mode = "appData";
std::string data_dir_override;
std::string allowed_origin;
std::string start_url;
int window_width = 1200;
int window_height = 800;
std::vector<std::uint8_t> bundle_bytes;
std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> bundle_files;
bool development = false;
bool devtools = false;
bool bundled = false;
std::string titlebar_color;
std::string titlebar_text_color;

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
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
  return std::filesystem::weakly_canonical(buffer.data());
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
  if (!has_magic(footer, "TINYEND1") || !read_u64(footer + 8, &header) || header > footer ||
      !has_magic(static_cast<std::size_t>(header), "TINYBND1") ||
      !read_u64(static_cast<std::size_t>(header) + 8, &manifest_size)) return false;
  const auto manifest_start = static_cast<std::size_t>(header) + 16;
  if (manifest_start > footer || manifest_size > footer - manifest_start) return false;

  NSData* manifest_data = [NSData dataWithBytes:bundle_bytes.data() + manifest_start length:static_cast<NSUInteger>(manifest_size)];
  NSError* error = nil;
  NSDictionary* manifest = [NSJSONSerialization JSONObjectWithData:manifest_data options:0 error:&error];
  if (error || ![manifest isKindOfClass:[NSDictionary class]]) return false;
  NSString* name = manifest[@"appName"];
  NSString* storage = manifest[@"storage"];
  if ([name isKindOfClass:[NSString class]]) app_name = [name UTF8String];
  if ([storage isKindOfClass:[NSString class]]) storage_mode = [storage UTF8String];
  NSNumber* width = manifest[@"windowWidth"];
  NSNumber* height = manifest[@"windowHeight"];
  if ([width isKindOfClass:[NSNumber class]]) window_width = width.intValue;
  if ([height isKindOfClass:[NSNumber class]]) window_height = height.intValue;
  NSString* titlebar = manifest[@"titlebarColor"];
  NSString* titlebar_text = manifest[@"titlebarTextColor"];
  if ([titlebar isKindOfClass:[NSString class]]) titlebar_color = [titlebar UTF8String];
  if ([titlebar_text isKindOfClass:[NSString class]]) titlebar_text_color = [titlebar_text UTF8String];
  const auto data_start = manifest_start + static_cast<std::size_t>(manifest_size);
  for (NSDictionary* entry in manifest[@"files"]) {
    NSString* path = entry[@"path"];
    NSNumber* offset = entry[@"offset"];
    NSNumber* length = entry[@"size"];
    if (![path isKindOfClass:[NSString class]] || ![offset isKindOfClass:[NSNumber class]] ||
        ![length isKindOfClass:[NSNumber class]]) return false;
    const auto file_offset = offset.unsignedLongLongValue;
    const auto file_size = length.unsignedLongLongValue;
    if (file_offset > footer - data_start || file_size > footer - data_start - file_offset) return false;
    bundle_files[[path UTF8String]] = {data_start + file_offset, file_size};
  }
  bundled = true;
  return true;
}

std::string request_path(NSURL* url) {
  NSString* path_value = [url.path stringByRemovingPercentEncoding];
  std::string path = path_value ? [path_value UTF8String] : "";
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  return path.empty() ? "index.html" : path;
}

std::string content_type(const std::string& path) {
  const auto value = lower(path);
  if (ends_with(value, ".html")) return "text/html";
  if (ends_with(value, ".js")) return "text/javascript";
  if (ends_with(value, ".css")) return "text/css";
  if (ends_with(value, ".json")) return "application/json";
  if (ends_with(value, ".svg")) return "image/svg+xml";
  if (ends_with(value, ".png")) return "image/png";
  if (ends_with(value, ".jpg") || ends_with(value, ".jpeg")) return "image/jpeg";
  if (ends_with(value, ".gif")) return "image/gif";
  if (ends_with(value, ".webp")) return "image/webp";
  if (ends_with(value, ".woff2")) return "font/woff2";
  return "application/octet-stream";
}

NSString* json_text(id value) {
  NSError* error = nil;
  NSData* data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:&error];
  if (error) return @"null";
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"null";
}

void send_message(NSString* identifier, id result, NSString* code = nil, NSString* message = nil) {
  if (!webview) return;
  NSMutableDictionary* response = [@{ @"id": identifier ?: @"" } mutableCopy];
  if (code) response[@"error"] = @{ @"code": code, @"message": message ?: @"Tiny request failed." };
  else response[@"result"] = result ?: [NSNull null];
  const auto script = [NSString stringWithFormat:@"window.__tinyDeliver(%@);", json_text(response)];
  [webview evaluateJavaScript:script completionHandler:nil];
}

bool valid_store(NSString* store) {
  if (![store isKindOfClass:[NSString class]] || store.length == 0 || store.length > 64) return false;
  for (NSUInteger index = 0; index < store.length; ++index) {
    const unichar character = [store characterAtIndex:index];
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-') return false;
  }
  return true;
}

std::filesystem::path store_path(NSString* identifier, NSDictionary* request) {
  NSString* store = request[@"store"];
  if (!valid_store(store)) {
    send_message(identifier, nil, @"INVALID_STORE", @"Store names may contain only letters, numbers, hyphens, and underscores.");
    return {};
  }
  return data_dir / (std::string([store UTF8String]) + ".json");
}

void handle_message(NSString* json) {
  NSData* bytes = [json dataUsingEncoding:NSUTF8StringEncoding];
  NSError* error = nil;
  NSDictionary* request = [NSJSONSerialization JSONObjectWithData:bytes options:NSJSONReadingAllowFragments error:&error];
  if (error || ![request isKindOfClass:[NSDictionary class]]) return;
  NSString* identifier = request[@"id"];
  NSString* method = request[@"method"];
  if (![identifier isKindOfClass:[NSString class]] || ![method isKindOfClass:[NSString class]]) return;

  if ([method isEqualToString:@"app.getDataPath"]) return send_message(identifier, [NSString stringWithUTF8String:data_dir.c_str()]);
  if ([method isEqualToString:@"window.close"]) {
    [window_handle performClose:nil];
    return send_message(identifier, [NSNull null]);
  }
  if ([method isEqualToString:@"window.minimize"]) {
    [window_handle miniaturize:nil];
    return send_message(identifier, [NSNull null]);
  }
  if ([method isEqualToString:@"window.maximize"]) {
    [window_handle zoom:nil];
    return send_message(identifier, [NSNull null]);
  }
  if ([method isEqualToString:@"shell.openExternal"]) {
    NSString* value = request[@"url"];
    if (![value isKindOfClass:[NSString class]] || (![value hasPrefix:@"https://"] && ![value hasPrefix:@"http://"])) {
      return send_message(identifier, nil, @"INVALID_URL", @"Only HTTP and HTTPS URLs can be opened.");
    }
    NSURL* url = [NSURL URLWithString:value];
    if (!url || ![[NSWorkspace sharedWorkspace] openURL:url]) {
      return send_message(identifier, nil, @"OPEN_FAILED", @"The system browser could not open the URL.");
    }
    return send_message(identifier, [NSNull null]);
  }
  if ([method isEqualToString:@"data.read"]) {
    const auto path = store_path(identifier, request);
    if (path.empty()) return;
    NSData* value = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
    if (!value) return send_message(identifier, [NSNull null]);
    NSError* read_error = nil;
    id parsed = [NSJSONSerialization JSONObjectWithData:value options:NSJSONReadingAllowFragments error:&read_error];
    if (read_error || !parsed) return send_message(identifier, nil, @"INVALID_JSON", @"The JSON store is not valid JSON.");
    return send_message(identifier, parsed);
  }
  if ([method isEqualToString:@"data.write"]) {
    const auto path = store_path(identifier, request);
    if (path.empty()) return;
    id value = request[@"value"];
    if (!value) return send_message(identifier, nil, @"INVALID_JSON", @"A JSON value is required.");
    NSError* write_error = nil;
    NSData* encoded = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:&write_error];
    if (write_error || ![encoded writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingAtomic error:&write_error]) {
      return send_message(identifier, nil, @"WRITE_FAILED", @"The JSON store could not be saved.");
    }
    return send_message(identifier, [NSNull null]);
  }
  send_message(identifier, nil, @"METHOD_NOT_FOUND", @"The native method does not exist.");
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

NSColor* color_from_hex(const std::string& value) {
  if (value.size() != 7 || value[0] != '#') return nil;
  unsigned int red = 0, green = 0, blue = 0;
  if (std::sscanf(value.c_str(), "#%02x%02x%02x", &red, &green, &blue) != 3) return nil;
  return [NSColor colorWithSRGBRed:red / 255.0 green:green / 255.0 blue:blue / 255.0 alpha:1.0];
}

} // namespace

@interface TinySchemeHandler : NSObject <WKURLSchemeHandler>
@end

@implementation TinySchemeHandler
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
  const auto path = request_path(task.request.URL);
  const auto found = bundle_files.find(path);
  if (found == bundle_files.end()) {
    NSError* error = [NSError errorWithDomain:@"Tiny" code:404 userInfo:@{NSLocalizedDescriptionKey: @"Resource not found."}];
    [task didFailWithError:error];
    return;
  }
  const auto [offset, size] = found->second;
  NSData* data = [NSData dataWithBytes:bundle_bytes.data() + offset length:static_cast<NSUInteger>(size)];
  NSURLResponse* response = [[NSURLResponse alloc] initWithURL:task.request.URL MIMEType:[NSString stringWithUTF8String:content_type(path).c_str()]
      expectedContentLength:static_cast<NSInteger>(size) textEncodingName:@"utf-8"];
  [task didReceiveResponse:response];
  [task didReceiveData:data];
  [task didFinish];
}
- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {}
@end

@interface TinyScriptHandler : NSObject <WKScriptMessageHandler>
@end

@implementation TinyScriptHandler
- (void)userContentController:(WKUserContentController*)userContentController didReceiveScriptMessage:(WKScriptMessage*)message {
  if ([message.body isKindOfClass:[NSString class]]) handle_message(message.body);
}
@end

@interface TinyAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
@property(nonatomic, strong) TinyScriptHandler* scriptHandler;
@property(nonatomic, strong) TinySchemeHandler* schemeHandler;
@end

@implementation TinyAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  NSRect frame = NSMakeRect(0, 0, window_width, window_height);
  window_handle = [[NSWindow alloc] initWithContentRect:frame
      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
      backing:NSBackingStoreBuffered defer:NO];
  window_handle.title = [NSString stringWithUTF8String:app_name.c_str()];
  window_handle.delegate = self;
  [window_handle center];

  WKWebViewConfiguration* configuration = [WKWebViewConfiguration new];
  self.schemeHandler = [TinySchemeHandler new];
  [configuration setURLSchemeHandler:self.schemeHandler forURLScheme:@"tiny"];
  self.scriptHandler = [TinyScriptHandler new];
  WKUserContentController* userContentController = [WKUserContentController new];
  [userContentController addUserScript:[[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:bridge_script]
      injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
  [userContentController addScriptMessageHandler:self.scriptHandler name:@"tiny"];
  configuration.userContentController = userContentController;
  configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
  if (devtools) [configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

  webview = [[WKWebView alloc] initWithFrame:window_handle.contentView.bounds configuration:configuration];
  webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  webview.navigationDelegate = self;
  webview.UIDelegate = self;
  if (devtools) {
    if (@available(macOS 13.3, *)) webview.inspectable = YES;
  }
  window_handle.contentView = webview;

  NSColor* titlebar = color_from_hex(titlebar_color);
  if (titlebar) {
    window_handle.titlebarAppearsTransparent = YES;
    window_handle.backgroundColor = titlebar;
  }
  NSColor* text = color_from_hex(titlebar_text_color);
  if (text) {
    const auto luminance = text.redComponent * 0.299 + text.greenComponent * 0.587 + text.blueComponent * 0.114;
    window_handle.appearance = [NSAppearance appearanceNamed:luminance < 0.5 ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua];
  }
  [window_handle makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  const std::string initial_url = development ? start_url : "tiny://app/index.html";
  NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:initial_url.c_str()]];
  [webview loadRequest:[NSURLRequest requestWithURL:url]];
}

- (void)windowWillClose:(NSNotification*)notification { [NSApp terminate:nil]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }
- (void)webView:(WKWebView*)view decidePolicyForNavigationAction:(WKNavigationAction*)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
  const auto url = action.request.URL.absoluteString;
  const auto value = url ? [url UTF8String] : "";
  decisionHandler(same_origin(value, development ? allowed_origin : "tiny://app") ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView*)view didFinishNavigation:(WKNavigation*)navigation {
  if (devtools) {
    if (@available(macOS 13.3, *)) view.inspectable = YES;
  }
}
- (WKWebView*)webView:(WKWebView*)view createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
    forNavigationAction:(WKNavigationAction*)navigationAction windowFeatures:(WKWindowFeatures*)windowFeatures { return nil; }
@end

void parse_arguments(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dev" && index + 1 < argc) {
      development = true;
      start_url = argv[++index];
      allowed_origin = origin_of(start_url);
    } else if (argument == "--app-name" && index + 1 < argc) app_name = argv[++index];
    else if (argument == "--storage" && index + 1 < argc) storage_mode = argv[++index];
    else if (argument == "--data-dir" && index + 1 < argc) data_dir_override = argv[++index];
    else if (argument == "--window-width" && index + 1 < argc) window_width = std::stoi(argv[++index]);
    else if (argument == "--window-height" && index + 1 < argc) window_height = std::stoi(argv[++index]);
    else if (argument == "--titlebar-color" && index + 1 < argc) titlebar_color = argv[++index];
    else if (argument == "--titlebar-text-color" && index + 1 < argc) titlebar_text_color = argv[++index];
    else if (argument == "--devtools") devtools = true;
  }
}

int main(int argc, char** argv) {
  @autoreleasepool {
    parse_arguments(argc, argv);
    executable_path = module_path();
    executable_dir = executable_path.parent_path();
    if (!development && !load_bundle()) return 1;
    app_name = safe_name(app_name);
    if (storage_mode != "portable") storage_mode = "appData";
    if (data_dir_override.empty()) {
      NSArray* paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
      data_dir = storage_mode == "portable"
          ? executable_dir / "data"
          : std::filesystem::path([paths.firstObject UTF8String]) / safe_name(app_name);
    } else data_dir = std::filesystem::absolute(data_dir_override);
    std::error_code error;
    std::filesystem::create_directories(data_dir, error);
    if (error) return 1;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    TinyAppDelegate* delegate = [TinyAppDelegate new];
    NSApp.delegate = delegate;
    [NSApp run];
  }
  return 0;
}
