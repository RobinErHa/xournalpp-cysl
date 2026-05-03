/*
 * Xournal++ — PDF render helper
 *
 * Renders ONE page of a PDF to a PNG file, then exits. Designed to be
 * spawned by Xournal++ inside a firejail sandbox so that any RCE or
 * crash in Poppler is contained to this short-lived process.
 *
 * Usage:
 *     xopp-render-helper <input.pdf> <page-index> <zoom> <output.png>
 *
 * Arguments:
 *     input.pdf    Absolute path to the PDF file. Must be readable
 *                  inside the sandbox (the caller is responsible for
 *                  whitelisting it via firejail).
 *     page-index   Zero-based page number.
 *     zoom         Render scale factor (1.0 = native PDF resolution,
 *                  matching Xournal++'s PdfCache "renderZoom" semantics).
 *                  Note this is a *cairo scale factor*, not a DPI ratio.
 *     output.png   Path where the rendered PNG will be written. The
 *                  caller is responsible for whitelisting the directory
 *                  inside the sandbox.
 *
 * Exit codes:
 *      0   Success — PNG was written.
 *      1   Argument / usage error.
 *      2   Failed to open the PDF (Poppler returned an error).
 *      3   Page index out of range.
 *      4   Cairo or PNG write error.
 *    >127  Killed by signal (e.g., Poppler segfaulted on a malicious PDF).
 *          The parent detects this via WIFSIGNALED and treats the page
 *          as un-renderable.
 *
 * Developed with assistance from Claude Sonnet 4.6.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cairo.h>
#include <glib.h>
#include <poppler.h>

namespace {

constexpr int EXIT_USAGE = 1;
constexpr int EXIT_OPEN_FAILED = 2;
constexpr int EXIT_BAD_PAGE = 3;
constexpr int EXIT_RENDER_FAILED = 4;

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s <input.pdf> <page-index> <zoom> <output.png>\n",
                 argv0);
}

/// Convert a filesystem path to a file
/// Returns an owned C string; caller must g_free().
gchar* pathToFileUri(const char* path) {
    GError* err = nullptr;
    gchar* uri = g_filename_to_uri(path, nullptr, &err);
    if (!uri) {
        std::fprintf(stderr, "xopp-render-helper: cannot build URI for '%s': %s\n",
                     path, err ? err->message : "unknown error");
        if (err) g_error_free(err);
    }
    return uri;
}

int pageIndexFromArgv(const char* pageStr) {
    const int maxNrOfPages = 1'000'000;
    char* endp = nullptr;
    errno = 0;
    long pageIndexL = std::strtol(pageStr, &endp, 10);
    if (errno != 0 || endp == pageStr || *endp != '\0' || pageIndexL < 0 ||
        pageIndexL > maxNrOfPages) {
        std::fprintf(stderr, "xopp-render-helper: bad page index '%s'\n", pageStr);
        return -1;
    }
    const int pageIndex = static_cast<int>(pageIndexL);
    return pageIndex;
}

double zoomFromArgv(const char* zoomStr){
    char* endp = nullptr;
    errno = 0;
    double zoom = std::strtod(zoomStr, &endp);
    if (errno != 0 || endp == zoomStr || *endp != '\0' ||
        !(zoom > 0.0) || zoom > 100.0) {
        std::fprintf(stderr, "xopp-render-helper: bad zoom '%s'\n", zoomStr);
        return -1.00;
    }
    return zoom;
}

PopplerDocument* docFromUriPath(const char* pdfPath){

    gchar* uri = pathToFileUri(pdfPath);
    if (!uri) return nullptr;

    GError* err = nullptr;
    PopplerDocument* doc = poppler_document_new_from_file(uri, "", &err);
    if (!doc) {
        std::fprintf(stderr, "xopp-render-helper: poppler open failed: %s\n",
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
    }

    g_free(uri);
    return doc;
}

PopplerPage* getPageFromDoc(PopplerDocument* doc, int pageIndex) {
    const int nPages = poppler_document_get_n_pages(doc);
    if (pageIndex >= nPages) {
        std::fprintf(stderr, "xopp-render-helper: page %d out of range (doc has %d)\n",
                     pageIndex, nPages);
        g_object_unref(doc);
        return nullptr;
    }

    PopplerPage* page = poppler_document_get_page(doc, pageIndex);
    if (!page) {
        std::fprintf(stderr, "xopp-render-helper: cannot get page %d\n", pageIndex);
        g_object_unref(doc);
    }
    return page;
}

cairo_surface_t* makeSurface(PopplerPage *page, PopplerDocument* doc, double zoom ) {
    double pageW = 0.0, pageH = 0.0;
    poppler_page_get_size(page, &pageW, &pageH);

    const int outW = static_cast<int>(pageW * zoom + 0.5);
    const int outH = static_cast<int>(pageH * zoom + 0.5);

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, outW, outH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr, "xopp-render-helper: surface alloc failed: %s\n",
                     cairo_status_to_string(cairo_surface_status(surface)));
        cairo_surface_destroy(surface);
        g_object_unref(page);
        g_object_unref(doc);
        return nullptr;
    }
    return surface;

}

//The dangerous call we want to isolate with firejail poppler_page_render(..)
cairo_status_t renderPDF(cairo_surface_t* surface, PopplerPage* page, PopplerDocument* doc, double zoom){

    cairo_t* cr = cairo_create(surface);

    // Draw a white background (matching xournalpp)
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_scale(cr, zoom, zoom);

    poppler_page_render(page, cr);

    cairo_status_t cstatus = cairo_status(cr);
    cairo_destroy(cr);

    if (cstatus != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr, "xopp-render-helper: cairo error after render: %s\n",
                     cairo_status_to_string(cstatus));
        cairo_surface_destroy(surface);
        g_object_unref(page);
        g_object_unref(doc);
    }

    return cstatus;
}

cairo_status_t writePNG(cairo_surface_t* surface, PopplerPage* page, PopplerDocument* doc, const char* outPath) {
    cairo_status_t wstatus = cairo_surface_write_to_png(surface, outPath);
    cairo_surface_destroy(surface);
    g_object_unref(page);
    g_object_unref(doc);

    if (wstatus != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr, "xopp-render-helper: PNG write failed: %s\n",
                     cairo_status_to_string(wstatus));
    }
    return wstatus;
}


}


int main(int argc, char** argv) {
    if (argc != 5) {
        usage(argv[0] ? argv[0] : "xopp-render-helper");
        return EXIT_USAGE;
    }

    const char* pdfPath = argv[1];
    const char* pageStr = argv[2];
    const char* zoomStr = argv[3];
    const char* outPath = argv[4];

    //Parse numeric arguments
    const int pageIndex = pageIndexFromArgv(pageStr);
    if (pageIndex < 0) return EXIT_USAGE;

    double zoom = zoomFromArgv(zoomStr);
    if (zoom < 0) return EXIT_USAGE;

    //Open PDF
    //For the PDF's file metadata
    PopplerDocument* doc = docFromUriPath(pdfPath);
    if (!doc) return EXIT_OPEN_FAILED;

    //Get Page
    //For the PDF's page metadata
    PopplerPage* page = getPageFromDoc(doc, pageIndex);
    if (!page) return EXIT_BAD_PAGE;

    //Create the needed surface
    //Allocates the pixel buffer to write to
    cairo_surface_t* surface = makeSurface(page, doc, zoom);
    if (!surface) return EXIT_RENDER_FAILED;

    //Render the pdf
    //Paint the page contents to the Pixel buffer
    cairo_status_t renderStatus = renderPDF(surface, page, doc, zoom);
    if (renderStatus != CAIRO_STATUS_SUCCESS) return EXIT_RENDER_FAILED;

    //Write PNG
    //Encode the buffer to the  PNG-file
    cairo_status_t pngStatus = writePNG(surface, page, doc, outPath);
    if (pngStatus != CAIRO_STATUS_SUCCESS) return EXIT_RENDER_FAILED;

    return 0;
}
