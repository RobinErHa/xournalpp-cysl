#include "PdfCache.h"

#include <algorithm>  // for max
#include <cmath>      // for ceil, abs
#include <cstdio>     // for size_t
#include <memory>     // for shared_ptr, __shared_ptr_access
#include <string>     // for string
#include <utility>    // for move

#include <glib.h>  // for g_warning

#include "control/settings/Settings.h"  // for Settings
#include "pdf/base/XojPdfDocument.h"    // for XojPdfDocument
#include "util/Range.h"                 // for Range
#include "util/i18n.h"                  // for _
#include "util/safe_casts.h"            // for as_unsigned
#include "view/Mask.h"                  // for Mask

#include <fcntl.h>      // O_WRONLY
#include <spawn.h>      // posix_spawnp, posix_spawn_file_actions_*
#include <sys/wait.h>   // waitpid, WIFEXITED, WEXITSTATUS, WIFSIGNALED, WTERMSIG
#include <unistd.h>     // close, unlink, mkstemps, STDOUT_FILENO, STDERR_FILENO, environ


class PdfCacheEntry {
public:
    /**
     *   Cache [img], the result of rendering [popplerPage] with
     * the given [zoom].
     *  A change in the document's zoom causes a change in the
     * quality of the PDF backgrounds (zoomed in => need a higher
     * quality rendering).
     *
     * @param popplerPage
     * @param buffer is the result of rendering popplerPage
     */
    PdfCacheEntry(XojPdfPageSPtr popplerPage, xoj::view::Mask&& buffer):
            popplerPage(std::move(popplerPage)), buffer(std::forward<xoj::view::Mask>(buffer)) {}

    ~PdfCacheEntry() = default;

    XojPdfPageSPtr popplerPage;
    xoj::view::Mask buffer;
};

PdfCache::PdfCache(const XojPdfDocument& doc, Settings* settings, fs::path pdfFilepath): pdfDocument(doc), pdfFilepath(std::move(pdfFilepath)) { updateSettings(settings); }

PdfCache::~PdfCache() = default;

void PdfCache::setRefreshThreshold(double threshold) { this->zoomRefreshThreshold = threshold; }

void PdfCache::setMaxSize(size_t newSize) {
    this->maxSize = newSize;
    if (this->data.size() > this->maxSize) {
        this->data.resize(this->maxSize);
    }
}

void PdfCache::updateSettings(Settings* settings) {
    if (settings) {
        setMaxSize(as_unsigned(settings->getPdfPageCacheSize()));
        setRefreshThreshold(settings->getPDFPageRerenderThreshold());
    }
}

auto PdfCache::lookup(size_t pdfPageNo) const -> const PdfCacheEntry* {
    for (auto& e: this->data) {
        if (static_cast<size_t>(e->popplerPage->getPageId()) == pdfPageNo) {
            return e.get();
        }
    }

    return nullptr;
}

auto PdfCache::cache(XojPdfPageSPtr popplerPage, xoj::view::Mask&& buffer) -> const PdfCacheEntry* {
    if (this->data.size() > this->maxSize) {
        this->data.resize(this->maxSize);
    }

    this->data.emplace_front(
            std::make_unique<PdfCacheEntry>(std::move(popplerPage), std::forward<xoj::view::Mask>(buffer)));

    return this->data.front().get();
}

void PdfCache::render(cairo_t* cr, size_t pdfPageNo, double zoom, double pageWidth, double pageHeight) {
    std::lock_guard<std::mutex> lock(this->renderMutex);

    //Call Helper binary with firejail if env variable is set correctly:
    if (!this->pdfFilepath.empty() && std::getenv("XOPP_RENDER_HELPER")) {
        if (renderViaFirejail(cr, pdfPageNo, zoom, pageWidth, pageHeight)) {
            return;
        }
        renderMissingPdfPage(cr, pageWidth, pageHeight);
        return;
    }


    const PdfCacheEntry* cacheResult = lookup(pdfPageNo);

    bool needsRefresh = cacheResult == nullptr;

    if (!needsRefresh) {
        double averagedZoom = (zoom + cacheResult->buffer.getZoom()) / 2.0;
        double percentZoomChange = std::abs(cacheResult->buffer.getZoom() - zoom) * 100.0 / averagedZoom;

        // If we do have a cached result, is its rendering quality
        // acceptable for our current zoom?
        needsRefresh = (zoom > 1.0 && percentZoomChange > this->zoomRefreshThreshold);
    }

    if (needsRefresh) {
        double renderZoom = std::max(zoom, 1.0);

        auto popplerPage = cacheResult ? cacheResult->popplerPage : pdfDocument.getPage(pdfPageNo);

        if (!popplerPage) {
            g_warning("PdfCache::render Could not get the pdf page %zu from the document", pdfPageNo);
            renderMissingPdfPage(cr, pageWidth, pageHeight);
            return;
        }

        xoj::view::Mask buffer(cairo_get_target(cr), Range(0, 0, popplerPage->getWidth(), popplerPage->getHeight()),
                               renderZoom, CAIRO_CONTENT_COLOR_ALPHA);
        popplerPage->render(buffer.get());
        cacheResult = cache(popplerPage, std::move(buffer));
    }

    cacheResult->buffer.paintTo(cr);
}

//Anonymous namespace for firejail helper functions
namespace {


// Build a minimal environment for the helper. Firejail caps env at 256
// vars, and our parent process can exceed that.
std::vector<std::string> buildFirejailEnv(){
    auto pickEnv = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? std::string(name) + "=" + v : std::string{};
    };

    std::vector<std::string> envOwned;
    for (const char* var : {"PATH", "HOME", "LANG", "LC_ALL", "LC_CTYPE",
                            "XDG_RUNTIME_DIR", "TMPDIR", "FONTCONFIG_PATH"}) {
        auto kv = pickEnv(var);
        if (!kv.empty()) envOwned.push_back(std::move(kv));
    }
    return envOwned;
}

std::vector<std::string> setArgvForFirejail(bool useFirejail, const char* firejailProfile, const char* helperEnv, const std::string& pdfPathStr, const std::string& pageStr, const std::string& zoomStr, const std::string& tmpl){
    std::vector<std::string> argvOwned;
     if (useFirejail) {
            argvOwned.emplace_back("firejail");
            argvOwned.emplace_back("--quiet");
            argvOwned.emplace_back(std::string("--profile=") + firejailProfile);
            argvOwned.emplace_back(std::string("--whitelist=") + pdfPathStr);
            argvOwned.emplace_back("--");

            //For showcasing Firejail:
            argvOwned.emplace_back("strace");
            argvOwned.emplace_back("-f");
            argvOwned.emplace_back("-o");
            argvOwned.emplace_back("/tmp/helper-strace.log");

            argvOwned.emplace_back(helperEnv);
            argvOwned.emplace_back(pdfPathStr);
            argvOwned.emplace_back(pageStr);
            argvOwned.emplace_back(zoomStr);
            argvOwned.emplace_back(tmpl);
        } else {
            argvOwned.emplace_back(helperEnv);
            argvOwned.emplace_back(pdfPathStr);
            argvOwned.emplace_back(pageStr);
            argvOwned.emplace_back(zoomStr);
            argvOwned.emplace_back(tmpl);
        }

    return argvOwned;
}

bool runFireJailProcess(std::vector<char*>& argv, std::string& tmpl, int& outStatus) {

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    //Setup Firejail environment
    auto envOwned = buildFirejailEnv();
    std::vector<char*> envp;
    envp.reserve(envOwned.size() + 1);
    for (auto& s : envOwned) envp.push_back(s.data());
    envp.push_back(nullptr);


    //Spawn Firejail process with the firejail environment:
    pid_t pid = -1;
    int spawnRc = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawnRc != 0) {
        g_warning("renderViaFirejail spawn(%s) failed: %s", argv[0], std::strerror(spawnRc));
        ::unlink(tmpl.c_str()); //cleanup tmpl
        return false;
    }

    //Wait for process to complete
    //On success: page is saved as png in `tmpl`
    while (::waitpid(pid, &outStatus, 0) == -1 && errno == EINTR) {}

    return true;
}

}


bool PdfCache::renderViaFirejail(cairo_t* target, size_t pdfPageNo, double zoom,
                                 double /*pageWidth*/, double /*pageHeight*/) {

    // Create a writable buffer for mkstemps to rewrite the XXXXXX suffix.
    std::string tmpl = "/tmp/xopp-render-XXXXXX.png";
    int fd = ::mkstemps(tmpl.data(), 4);
    if (fd < 0) {
        g_warning("PdfCache::renderViaFirejail mkstemps failed: %s", std::strerror(errno));
        return false;
    }
    ::close(fd);

    // Get the value of the render-helper env variable
    // Abort if it is not set
    const char* helperEnv = std::getenv("XOPP_RENDER_HELPER");
    if (!helperEnv) {
        ::unlink(tmpl.c_str());
        return false;
    }

    const std::string pdfPathStr = this->pdfFilepath.string();
    const std::string pageStr = std::to_string(pdfPageNo);
    std::string zoomStr;
    {
        std::array<char, 64> zoomBuf{};
        std::snprintf(zoomBuf.data(), zoomBuf.size(), "%.6f", zoom);
        zoomStr.assign(zoomBuf.data());
    }

    //Get the value of the firejail profile env variable
    const char* firejailProfile = std::getenv("XOPP_RENDER_FIREJAIL_PROFILE");
    const bool useFirejail = (firejailProfile && firejailProfile[0]);

    //Prepare argv for spawning the firejail process
    std::vector<std::string> argvOwned = setArgvForFirejail(useFirejail, firejailProfile, helperEnv, pdfPathStr, pageStr, zoomStr, tmpl);

    // posix_spawnp wants `char* const argv[]` with a trailing nullptr.
    // Build a vector<char*> into our owned strings' mutable storage
    std::vector<char*> argv;
    argv.reserve(argvOwned.size() + 1);
    for (auto& s : argvOwned) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    //Spawn the firejal process and wait for the return
    int status = 0;
    bool spawnedSuccessfully = runFireJailProcess(argv, tmpl, status);
    if (!spawnedSuccessfully) return false;

    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFSIGNALED(status)) {
        g_warning("renderViaFirejail page %zu killed by signal %d", pdfPageNo, WTERMSIG(status));
    } else if (!ok) {
        g_warning("renderViaFirejail page %zu exit %d", pdfPageNo, WEXITSTATUS(status));
    }
    if (!ok) {
        ::unlink(tmpl.c_str());
        return false;
    }

    //Create the surface from the png
    cairo_surface_t* png = cairo_image_surface_create_from_png(tmpl.c_str());
    ::unlink(tmpl.c_str());
    if (cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
        g_warning("renderViaFirejail PNG load failed: %s",
                  cairo_status_to_string(cairo_surface_status(png)));
        cairo_surface_destroy(png);
        return false;
    }

    //draw the surface in the cairo context
    cairo_save(target);
    cairo_identity_matrix(target);
    cairo_set_source_surface(target, png, 0, 0);
    cairo_set_operator(target, CAIRO_OPERATOR_SOURCE);
    cairo_paint(target);
    cairo_restore(target);

    //cleanup
    cairo_surface_destroy(png);
    g_message("renderViaFirejail page %zu rendered successfully", pdfPageNo);
    return true;
}

void PdfCache::renderMissingPdfPage(cairo_t* cr, double pageWidth, double pageHeight) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 26);

    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);

    cairo_text_extents_t extents = {0};
    std::string strMissing = _("PDF background missing");

    cairo_text_extents(cr, strMissing.c_str(), &extents);
    cairo_move_to(cr, pageWidth / 2 - extents.width / 2, pageHeight / 2 - extents.height / 2);
    cairo_text_path(cr, strMissing.c_str());
}
