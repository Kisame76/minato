// apk-run's fake WebView host (--webview-fake, ADR-0056 step b2): it answers the guest's WebViews where no host has a
// WebView, playing the effects of webview-test's pages as Android 15's WebView shows them on apk-runner-35
// (fixtures/expected/webview-test.logcat), so that the guest's provider, its channel and its JNI are tested before the
// App's WKWebView host (step c). It runs no page and no script: it knows the asset page.html (a title and an alert
// while it loads), answers any other asset with Chromium's file-not-found error page, takes a loadData or
// loadDataWithBaseURL page from its <title>, joining a CALL that came in parts, and answers the scripts "1 + 2" and
// "document.title". The records go out in Chromium's order, from a worker thread of its own, since a handler must not
// block; an alert waits for the guest's ANSWER, up to 10 s, as the host does.
#pragma once

#include "APKRuntime/apkrt.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace webview_fake {

/// The value of the JSON string field `name` of `json`, its escapes decoded (UTF-8 out); empty when absent.
/// `found` says whether the field is there as a string. Only for the guest's own objects, one level deep.
inline std::string string_field(const std::string &json, const std::string &name, bool *found = nullptr) {
    if (found != nullptr) *found = false;
    const std::string key = "\"" + name + "\":";
    std::size_t at = json.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    if (at >= json.size() || json[at] != '"') return {};
    std::string out;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            if (found != nullptr) *found = true;
            return out;
        }
        if (c != '\\' || i + 1 >= json.size()) {
            out += c;
            continue;
        }
        const char escaped = json[++i];
        switch (escaped) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 >= json.size()) return out;
                const unsigned code = static_cast<unsigned>(std::stoul(json.substr(i + 1, 4), nullptr, 16));
                i += 4;
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xc0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (code & 0x3f));
                }
                break;
            }
            default: out += escaped; break;  // \" \\ \/
        }
    }
    return out;
}

/// Whether `json` has the field `name` with the literal value `value` (a number or true/false).
inline bool literal_field(const std::string &json, const std::string &name, const std::string &value) {
    return json.find("\"" + name + "\":" + value) != std::string::npos;
}

/// `text` as a JSON string, quoted.
inline std::string quoted(const std::string &text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

/// One view's page and history, as Chromium keeps them.
struct Item {
    std::string url;
    std::string original;
    std::string title;
};

struct View {
    std::vector<Item> history;
    int index = -1;
    std::string parts;  // the cut field of a CALL that came in parts, so far
};

/// A job of the worker: a CALL of the guest, whole.
struct Job {
    std::uint64_t token;
    std::uint64_t request_id;
    std::string call;
};

class Host {
public:
    /// apkrt_session.webview: takes the request at once and leaves the records to the worker.
    void handle(const apkrt_webview_call &call) {
        std::lock_guard<std::mutex> hold(lock_);
        switch (call.op) {
            case APKRT_WEBVIEW_MAKE:
                views_[call.token] = View{};
                jobs_.push_back({call.token, 0, "{\"call\":\"created\"}"});
                break;
            case APKRT_WEBVIEW_PLACE:
                jobs_.push_back({call.token, 0, "{\"call\":\"placed\"}"});
                break;
            case APKRT_WEBVIEW_REMOVE:
                views_.erase(call.token);
                break;
            case APKRT_WEBVIEW_CALL:
                jobs_.push_back({call.token, call.request_id,
                                 std::string(static_cast<const char *>(call.payload), call.payload_length)});
                break;
            case APKRT_WEBVIEW_ANSWER:
                answers_[call.request_id] =
                        std::string(static_cast<const char *>(call.payload), call.payload_length);
                break;
        }
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        changed_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> hold(lock_);
        for (;;) {
            changed_.wait(hold, [this] { return !jobs_.empty(); });
            const Job job = jobs_.front();
            jobs_.pop_front();
            if (views_.count(job.token) == 0) continue;
            do_job(job, hold);
        }
    }

    void post(std::uint64_t token, std::uint32_t type, std::uint64_t request_id, const std::string &json) {
        apkrt_webview_post(token, type, request_id, json.data(), static_cast<std::uint32_t>(json.size()));
    }

    void navigation(std::uint64_t token, const char *phase, const std::string &url) {
        post(token, APKRT_WEBVIEW_REC_NAVIGATION, 0,
             std::string("{\"phase\":\"") + phase + "\",\"url\":" + quoted(url) + "}");
    }

    void progress(std::uint64_t token, int value) {
        post(token, APKRT_WEBVIEW_REC_PROGRESS, 0, "{\"progress\":" + std::to_string(value) + "}");
    }

    void title(std::uint64_t token, View &view, const std::string &text) {
        if (view.index >= 0) view.history[static_cast<std::size_t>(view.index)].title = text;
        post(token, APKRT_WEBVIEW_REC_TITLE, 0, "{\"title\":" + quoted(text) + "}");
    }

    /// A new entry after the current one, as a load commits, with the history the guest mirrors. The view's URL is
    /// the entry's, but for a page from loadDataWithBaseURL, whose entry has the data: URL and the view the base URL.
    void commit(std::uint64_t token, View &view, const Item &item, const std::string &url) {
        view.history.resize(static_cast<std::size_t>(view.index + 1));
        view.history.push_back(item);
        view.index = static_cast<int>(view.history.size()) - 1;
        std::string items;
        for (const Item &entry : view.history) {
            if (!items.empty()) items += ",";
            items += "{\"url\":" + quoted(entry.url) + ",\"originalUrl\":" + quoted(entry.original) +
                     ",\"title\":" + quoted(entry.title) + "}";
        }
        post(token, APKRT_WEBVIEW_REC_NAVIGATION, 0,
             "{\"phase\":\"committed\",\"url\":" + quoted(url) + ",\"originalUrl\":" + quoted(item.original) +
                     ",\"index\":" + std::to_string(view.index) + ",\"history\":[" + items + "]}");
    }

    /// The JS_DIALOG record, then the guest's ANSWER, which the handler files; without one in 10 s, the host's
    /// default goes on.
    void alert(std::uint64_t token, const std::string &url, const std::string &message,
               std::unique_lock<std::mutex> &hold) {
        const std::uint64_t id = next_record_++;
        post(token, APKRT_WEBVIEW_REC_JS_DIALOG, id,
             "{\"kind\":\"alert\",\"url\":" + quoted(url) + ",\"message\":" + quoted(message) + "}");
        changed_.wait_for(hold, std::chrono::seconds(10), [&] { return answers_.count(id) != 0; });
        answers_.erase(id);
    }

    void do_job(const Job &job, std::unique_lock<std::mutex> &hold) {
        View &view = views_[job.token];
        const std::string name = string_field(job.call, "call");
        if (name == "created") {
            post(job.token, APKRT_WEBVIEW_REC_CREATED, 0, "{}");
        } else if (name == "placed") {
            post(job.token, APKRT_WEBVIEW_REC_PLACED, 0, "{}");
        } else if (name == "loadUrl") {
            const std::string url = string_field(job.call, "url");
            navigation(job.token, "started", url);
            progress(job.token, 10);
            if (url.ends_with("://asset/page.html")) {
                commit(job.token, view, {url, url, ""}, url);
                title(job.token, view, "Minato page");
                alert(job.token, url, "hello from the page", hold);
                progress(job.token, 100);
                navigation(job.token, "finished", url);
            } else {
                // Chromium's error page, which commits after the failed load has finished.
                post(job.token, APKRT_WEBVIEW_REC_ERROR, 0,
                     "{\"mainFrame\":true,\"code\":-1,\"description\":\"net::ERR_FILE_NOT_FOUND\",\"url\":" +
                             quoted(url) + "}");
                navigation(job.token, "finished", url);
                commit(job.token, view, {url, url, ""}, url);
                title(job.token, view, "Webpage not available");
                progress(job.token, 100);
            }
        } else if (name == "loadData" || name == "loadDataWithBaseURL") {
            view.parts += string_field(job.call, "data");
            if (job.call.find("\"part\":") != std::string::npos && !literal_field(job.call, "last", "true")) return;
            const std::string data = std::move(view.parts);
            view.parts.clear();
            std::string base = string_field(job.call, "baseUrl");
            if (base.empty()) base = "about:blank";
            const std::string original = "data:text/html;charset=utf-8;base64,";
            navigation(job.token, "started", base);
            progress(job.token, 10);
            commit(job.token, view, {original, original, ""}, base);
            const std::size_t open = data.find("<title>");
            const std::size_t close = data.find("</title>");
            if (open != std::string::npos && close != std::string::npos && close > open) {
                title(job.token, view, data.substr(open + 7, close - open - 7));
            }
            progress(job.token, 100);
            navigation(job.token, "finished", base);
        } else if (name == "evaluateJavascript") {
            const std::string script = string_field(job.call, "script");
            std::string value = "null";
            if (script == "1 + 2") {
                value = "3";
            } else if (script == "document.title" && view.index >= 0) {
                value = quoted(view.history[static_cast<std::size_t>(view.index)].title);
            }
            post(job.token, APKRT_WEBVIEW_REC_JS_RESULT, job.request_id, "{\"json\":" + quoted(value) + "}");
        }
        // make, settings and the navigation calls change nothing this fake shows
    }

    std::mutex lock_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    std::map<std::uint64_t, View> views_;
    std::map<std::uint64_t, std::string> answers_;
    std::uint64_t next_record_ = 1;
    std::thread worker_;
};

inline Host &host() {
    static Host *one = new Host;  // lives to the process's end, as its worker does
    return *one;
}

/// The handler apk-run installs with --webview-fake.
inline void handle(void *, const apkrt_webview_call *call) {
    host().handle(*call);
}

}  // namespace webview_fake
