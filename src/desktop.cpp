#include "engine.h"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT_PTR timer_id = 1;
constexpr size_t max_details = 1000;
constexpr wchar_t window_class[] = L"QcopyDesktop.Window";
enum Id : int { source_list = 100, add_files, add_folder, remove_source, target_edit, browse, conflict_choice,
                verify_option, copy_action, move_action, cancel_action, issue_log };
template<class T> struct Release { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, Release<T>>;

std::wstring text(HWND window) {
    const int size = GetWindowTextLengthW(window);
    std::wstring value(size + 1, L'\0');
    value.resize(GetWindowTextW(window, value.data(), size + 1));
    return value;
}
std::wstring bytes(uint64_t amount) {
    constexpr const wchar_t* types[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(amount);
    size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(types)) { value /= 1024; ++unit; }
    wchar_t out[48]{};
    swprintf_s(out, unit ? L"%.1f %ls" : L"%.0f %ls", value, types[unit]);
    return out;
}

struct Work {
    qc::Progress progress;
    std::atomic_bool stop{}, done{};
    qc::Result result{};
    std::mutex mutex;
    std::vector<qc::Issue> details;
    size_t omitted{};
    size_t retained{};
    Clock::time_point started = Clock::now();
    std::jthread thread;
};

class Window {
    HWND window_{};
    HWND title_{}, hint_{}, list_{}, files_{}, folder_{}, remove_{}, target_label_{}, target_{}, browse_{};
    HWND conflict_label_{}, conflict_{}, verify_{}, copy_{}, move_{}, cancel_{}, bar_{}, status_{}, log_label_{}, log_{};
    HFONT body_{}, heading_{};
    UINT dpi_ = 96;
    std::vector<std::wstring> paths_;
    std::unique_ptr<Work> work_;
    bool close_when_done_{};

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND control(const wchar_t* type, const wchar_t* value, DWORD style, int id = 0, DWORD ex = 0) const {
        return CreateWindowExW(ex, type, value, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }
    void position(HWND item, int x, int y, int width, int height) const {
        MoveWindow(item, px(x), px(y), px(std::max(width, 1)), px(std::max(height, 1)), TRUE);
    }
    void fonts() {
        HFONT previous_body = body_, previous_heading = heading_;
        body_ = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        heading_ = CreateFontW(-px(29), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (auto item : {title_, hint_, list_, files_, folder_, remove_, target_label_, target_, browse_, conflict_label_,
                          conflict_, verify_, copy_, move_, cancel_, status_, log_label_, log_}) {
            SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(item == title_ ? heading_ : body_), TRUE);
        }
        if (previous_body) DeleteObject(previous_body);
        if (previous_heading) DeleteObject(previous_heading);
    }
    void layout() {
        RECT rect{};
        GetClientRect(window_, &rect);
        const int width = MulDiv(rect.right, 96, dpi_), height = MulDiv(rect.bottom, 96, dpi_);
        constexpr int edge = 22;
        constexpr int buttons = 116;
        const int content = width - 2 * edge;
        const int sources_h = std::max(107, (height - 255) * 53 / 100);
        const int target_y = 87 + sources_h + 15;
        const int options_y = target_y + 43;
        const int actions_y = options_y + 42;
        position(title_, edge, 16, content, 38);
        position(hint_, edge, 61, content, 21);
        position(list_, edge, 87, content - buttons - 9, sources_h);
        for (auto [index, item] : {std::pair{0, files_}, std::pair{1, folder_}, std::pair{2, remove_}})
            position(item, width - edge - buttons, 87 + 37 * index, buttons, 31);
        position(target_label_, edge, target_y + 6, 68, 23);
        position(target_, edge + 71, target_y, content - 158, 28);
        position(browse_, width - edge - 78, target_y, 78, 28);
        position(conflict_label_, edge, options_y + 5, 83, 23);
        position(conflict_, edge + 87, options_y, 135, 125);
        position(verify_, edge + 248, options_y, 230, 28);
        position(copy_, edge, actions_y, 108, 34);
        position(move_, edge + 118, actions_y, 108, 34);
        position(cancel_, width - edge - 96, actions_y, 96, 34);
        position(bar_, edge, actions_y + 47, content, 11);
        position(status_, edge, actions_y + 66, content, 22);
        position(log_label_, edge, actions_y + 92, content, 20);
        position(log_, edge, actions_y + 114, content, height - actions_y - 132);
    }
    void availability() const {
        const bool idle = !work_;
        for (auto item : {list_, files_, folder_, target_, browse_, conflict_, verify_}) EnableWindow(item, idle);
        EnableWindow(remove_, idle && SendMessageW(list_, LB_GETCURSEL, 0, 0) != LB_ERR);
        const bool ready = idle && !paths_.empty() && GetWindowTextLengthW(target_) != 0;
        EnableWindow(copy_, ready);
        EnableWindow(move_, ready);
        EnableWindow(cancel_, !idle && !work_->stop.load());
    }
    void add(std::wstring value) {
        if (value.empty() || std::find(paths_.begin(), paths_.end(), value) != paths_.end()) return;
        if (SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str())) < 0) return;
        const auto extent = std::min<size_t>(value.size(), 32000) * px(11);
        if (extent > static_cast<size_t>(SendMessageW(list_, LB_GETHORIZONTALEXTENT, 0, 0)))
            SendMessageW(list_, LB_SETHORIZONTALEXTENT, extent, 0);
        paths_.push_back(std::move(value));
    }
    void select(bool folders, bool destination) {
        IFileOpenDialog* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) return;
        const ComPtr<IFileOpenDialog> dialog(raw);
        DWORD options{};
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folders) options |= FOS_PICKFOLDERS;
        if (!destination) options |= FOS_ALLOWMULTISELECT;
        dialog->SetOptions(options);
        dialog->SetTitle(destination ? L"选择目标文件夹" : (folders ? L"添加文件夹" : L"添加文件"));
        if (FAILED(dialog->Show(window_))) return;
        IShellItemArray* raw_items = nullptr;
        if (FAILED(dialog->GetResults(&raw_items))) return;
        const ComPtr<IShellItemArray> items(raw_items);
        DWORD count{};
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* raw_item = nullptr;
            if (FAILED(items->GetItemAt(i, &raw_item))) continue;
            const ComPtr<IShellItem> item(raw_item);
            PWSTR name = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) continue;
            if (destination) SetWindowTextW(target_, name);
            else add(name);
            CoTaskMemFree(name);
        }
        availability();
    }
    void drop(HDROP drop_handle) {
        if (!work_) {
            const auto count = DragQueryFileW(drop_handle, 0xffffffff, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const auto n = DragQueryFileW(drop_handle, i, nullptr, 0);
                std::wstring value(n + 1, L'\0');
                value.resize(DragQueryFileW(drop_handle, i, value.data(), n + 1));
                add(std::move(value));
            }
            availability();
        }
        DragFinish(drop_handle);
    }
    void start(qc::Operation operation) {
        if (work_ || paths_.empty()) return;
        qc::Job job;
        job.sources = paths_;
        job.destination = text(target_);
        if (job.destination.empty()) return;
        job.operation = operation;
        const auto selected = SendMessageW(conflict_, CB_GETCURSEL, 0, 0);
        job.conflict = selected == 1 ? qc::Conflict::replace : selected == 2 ? qc::Conflict::rename : qc::Conflict::skip;
        job.verify = SendMessageW(verify_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        work_ = std::make_unique<Work>();
        auto& work = *work_;
        try {
            work.thread = std::jthread([job = std::move(job), &work] {
                work.result = qc::run(job, work.progress, work.stop, [&](const qc::Issue& issue) noexcept {
                    std::lock_guard lock(work.mutex);
                    if (work.retained < max_details) {
                        try { work.details.push_back(issue); ++work.retained; }
                        catch (const std::bad_alloc&) { ++work.omitted; }
                    } else ++work.omitted;
                });
                work.done.store(true, std::memory_order_release);
            });
        } catch (const std::system_error&) {
            work_.reset();
            MessageBoxW(window_, L"无法启动复制线程。", L"Qcopy", MB_OK | MB_ICONERROR);
            return;
        }
        SetWindowTextW(log_, L"");
        SetWindowTextW(status_, L"正在枚举文件…");
        SetWindowLongPtrW(bar_, GWL_STYLE, GetWindowLongPtrW(bar_, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(bar_, PBM_SETMARQUEE, TRUE, 65);
        SetTimer(window_, timer_id, 200, nullptr);
        availability();
        SetFocus(cancel_);
    }
    void append_details(Work& work, bool done) const {
        std::vector<qc::Issue> next;
        size_t omitted = 0;
        {
            std::lock_guard lock(work.mutex);
            next.swap(work.details);
            if (done) omitted = work.omitted;
        }
        if (next.empty() && !omitted) return;
        std::wstring content;
        for (const auto& issue : next) {
            content += issue.skipped ? L"已跳过  " : L"失败  ";
            content += issue.path + L"  ·  " + issue.stage + L"  (" + std::to_wstring(issue.code) +
                L")  " + qc::error_message(issue.code) + L"\r\n";
        }
        if (omitted) content += L"另有 " + std::to_wstring(omitted) + L" 条明细未显示。\r\n";
        SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
        SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(content.c_str()));
    }
    void tick() {
        if (!work_) return;
        auto& work = *work_;
        const bool done = work.done.load(std::memory_order_acquire);
        append_details(work, done);
        const auto completed = work.progress.completed.load();
        const auto moved = work.progress.transferred.load();
        const double seconds = std::max(0.001, std::chrono::duration<double>(Clock::now() - work.started).count());
        const bool known = work.progress.enumeration_done.load();
        const auto total = work.progress.discovered.load();
        std::wstring status;
        if (done) status = work.result.cancelled ? L"已取消 · " : (work.result.exit_code() ? L"已结束 · " : L"已完成 · ");
        else if (work.stop.load()) status = L"正在取消 · ";
        if (known || done) {
            if (GetWindowLongPtrW(bar_, GWL_STYLE) & PBS_MARQUEE) {
                SendMessageW(bar_, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrW(bar_, GWL_STYLE, GetWindowLongPtrW(bar_, GWL_STYLE) & ~PBS_MARQUEE);
            }
            const auto percent = total ? std::min<uint64_t>(100, completed * 100 / total) : (done ? 100 : 0);
            SendMessageW(bar_, PBM_SETPOS, percent, 0);
            if (!done) status += std::to_wstring(percent) + L"%  ·  ";
        }
        status += std::to_wstring(completed) + L" 项  ·  " + bytes(moved) + L"  ·  " +
            bytes(static_cast<uint64_t>(static_cast<double>(moved) / seconds)) + L"/s  ·  " +
            std::to_wstring(work.progress.failed.load()) + L" 失败  ·  " + std::to_wstring(work.progress.skipped.load()) + L" 跳过";
        SetWindowTextW(status_, status.c_str());
        if (!done) return;
        KillTimer(window_, timer_id);
        work_.reset();
        if (close_when_done_) { DestroyWindow(window_); return; }
        availability();
        if (IsWindowEnabled(copy_)) SetFocus(copy_);
    }
    void cancel() {
        if (!work_) return;
        work_->stop.store(true, std::memory_order_relaxed);
        availability();
    }
    bool create() {
        dpi_ = GetDpiForWindow(window_);
        title_ = control(L"STATIC", L"Qcopy", SS_LEFT);
        hint_ = control(L"STATIC", L"将文件或文件夹拖入窗口，或手动添加。", SS_LEFT);
        list_ = control(L"LISTBOX", nullptr, WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, source_list, WS_EX_CLIENTEDGE);
        files_ = control(L"BUTTON", L"添加文件…", WS_TABSTOP, add_files);
        folder_ = control(L"BUTTON", L"添加文件夹…", WS_TABSTOP, add_folder);
        remove_ = control(L"BUTTON", L"移除选中项", WS_TABSTOP, remove_source);
        target_label_ = control(L"STATIC", L"目标位置", SS_LEFT);
        target_ = control(L"EDIT", nullptr, WS_TABSTOP | ES_AUTOHSCROLL, target_edit, WS_EX_CLIENTEDGE);
        browse_ = control(L"BUTTON", L"浏览…", WS_TABSTOP, browse);
        conflict_label_ = control(L"STATIC", L"同名文件", SS_LEFT);
        conflict_ = control(WC_COMBOBOXW, nullptr, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, conflict_choice);
        for (const wchar_t* option : {L"跳过", L"覆盖文件", L"保留两份"})
            SendMessageW(conflict_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        SendMessageW(conflict_, CB_SETCURSEL, 0, 0);
        verify_ = control(L"BUTTON", L"复制后验证内容 (SHA-256)", WS_TABSTOP | BS_AUTOCHECKBOX, verify_option);
        copy_ = control(L"BUTTON", L"开始复制", WS_TABSTOP | BS_DEFPUSHBUTTON, copy_action);
        move_ = control(L"BUTTON", L"开始剪切", WS_TABSTOP, move_action);
        cancel_ = control(L"BUTTON", L"取消任务", WS_TABSTOP, cancel_action);
        bar_ = control(PROGRESS_CLASSW, nullptr, PBS_SMOOTH);
        status_ = control(L"STATIC", L"等待添加源文件和选择目标位置。", SS_LEFT);
        log_label_ = control(L"STATIC", L"失败和跳过明细", SS_LEFT);
        log_ = control(L"EDIT", nullptr, WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                       issue_log, WS_EX_CLIENTEDGE);
        SendMessageW(log_, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        if (!list_ || !target_ || !copy_ || !bar_ || !log_) return false;
        fonts();
        DragAcceptFiles(window_, TRUE);
        layout();
        availability();
        return true;
    }
    LRESULT message(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
        case WM_CREATE: return create() ? 0 : -1;
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {px(625), px(546)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            fonts();
            const auto* bounds = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_TIMER: if (wparam == timer_id) tick(); return 0;
        case WM_DROPFILES: drop(reinterpret_cast<HDROP>(wparam)); return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case source_list: case target_edit: availability(); break;
            case add_files: if (!work_) select(false, false); break;
            case add_folder: if (!work_) select(true, false); break;
            case browse: if (!work_) select(true, true); break;
            case remove_source: if (!work_) {
                const auto item = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                if (item != LB_ERR) {
                    SendMessageW(list_, LB_DELETESTRING, item, 0);
                    paths_.erase(paths_.begin() + item);
                }
                availability();
                break;
            } break;
            case copy_action: start(qc::Operation::copy); break;
            case move_action: start(qc::Operation::move); break;
            case cancel_action: cancel(); break;
            }
            return 0;
        case WM_CLOSE:
            if (work_) { close_when_done_ = true; cancel(); return 0; }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, msg, wparam, lparam);
    }
public:
    void load_launch_sources() {
        int count{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv) return;
        for (int i = 1; i < count; ++i) add(argv[i]);
        LocalFree(argv);
        availability();
    }
    ~Window() { if (body_) DeleteObject(body_); if (heading_) DeleteObject(heading_); }
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }
};
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof klass;
    klass.lpfnWndProc = Window::dispatch;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    klass.lpszClassName = window_class;
    if (!RegisterClassExW(&klass)) { if (com) CoUninitialize(); return 1; }
    Window state;
    const int dpi = static_cast<int>(GetDpiForSystem());
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, window_class, L"Qcopy · 文件复制与剪切",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(720, dpi, 96), MulDiv(610, dpi, 96), nullptr, nullptr, instance, &state);
    if (!window) { if (com) CoUninitialize(); return 1; }
    state.load_launch_sources();
    ShowWindow(window, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (com) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
