#include "RunReportPdf.h"

#if __has_include("AfarBuildInfo.h")
#include "AfarBuildInfo.h"
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#ifndef AFAR_SOFTWARE_VERSION
#define AFAR_SOFTWARE_VERSION "0.1.0"
#endif

#ifndef AFAR_CXX_COMPILER_ID
#define AFAR_CXX_COMPILER_ID "unknown"
#endif

#ifndef AFAR_CXX_COMPILER_VERSION
#define AFAR_CXX_COMPILER_VERSION "unknown"
#endif

#ifndef AFAR_CXX_STANDARD
#define AFAR_CXX_STANDARD "20"
#endif

#ifndef AFAR_QT_VERSION
#define AFAR_QT_VERSION "unknown"
#endif

#ifndef AFAR_NLOHMANN_JSON_VERSION
#define AFAR_NLOHMANN_JSON_VERSION "unknown"
#endif

// Пустая строка: git не был доступен при конфигурации CMake.
#ifndef AFAR_GIT_COMMIT
#define AFAR_GIT_COMMIT ""
#endif

namespace afar::report {
namespace {

std::string fromUtf8(std::u8string_view text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string pdfEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out.push_back('\\');
        }
        if (c < 32) {
            out.push_back('?');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string formatNumber(double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

std::string softwareVersion(const RunReportInfo& info)
{
    if (!info.software_version.empty()) {
        return info.software_version;
    }
    return AFAR_SOFTWARE_VERSION;
}

std::string buildCommitLine()
{
    const std::string_view hash{AFAR_GIT_COMMIT};
    if (hash.empty()) {
        return fromUtf8(u8"\u0445\u0435\u0448 \u043d\u0435 \u0432\u0448\u0438\u0442");
    }
    return std::string(hash);
}

std::string makeContentStream(const RunReportInfo& info)
{
    const std::string limits_note = fromUtf8(
        u8"\u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u041f\u041e, "
        u8"\u043d\u0435 \u0430\u0442\u0442\u0435\u0441\u0442\u043e\u0432\u0430\u043d\u043d\u0430\u044f "
        u8"\u043c\u0435\u0442\u0440\u043e\u043b\u043e\u0433\u0438\u044f");
    const std::string thru = fromUtf8(
        u8"\u043d\u0435 \u0438\u0437\u043c\u0435\u0440\u0435\u043d / "
        u8"\u0437\u043d\u0430\u0447\u0435\u043d\u0438\u044f \u043c\u0430\u0441\u0442\u0435\u0440\u0430");

    std::ostringstream body;
    body << "BT\n/F1 12 Tf\n50 760 Td\n"
         << "(AFAR RX Calibration Studio - series report) Tj\n"
         << "0 -16 Td\n(run_id: " << pdfEscape(info.run_id) << ") Tj\n"
         << "0 -16 Td\n(software_version: " << pdfEscape(softwareVersion(info)) << ") Tj\n"
         << "0 -16 Td\n(build_commit: " << pdfEscape(buildCommitLine()) << ") Tj\n"
         << "0 -16 Td\n(compiler: " << pdfEscape(AFAR_CXX_COMPILER_ID) << " "
         << pdfEscape(AFAR_CXX_COMPILER_VERSION) << ") Tj\n"
         << "0 -16 Td\n(cxx_standard: C++" << pdfEscape(AFAR_CXX_STANDARD) << ") Tj\n"
         << "0 -16 Td\n(qt_version: " << pdfEscape(AFAR_QT_VERSION) << ") Tj\n"
         << "0 -16 Td\n(nlohmann_json: " << pdfEscape(AFAR_NLOHMANN_JSON_VERSION) << ") Tj\n"
         << "0 -16 Td\n(completed_states: " << info.completed_states << ") Tj\n"
         << "0 -16 Td\n(valid_direct_count: " << info.valid_direct_count << ") Tj\n"
         << "0 -16 Td\n(series_path: " << pdfEscape(info.series_path) << ") Tj\n"
         << "0 -16 Td\n(limits: " << pdfEscape(limits_note) << ") Tj\n"
         << "0 -16 Td\n(max_drift_phase_deg: " << formatNumber(info.max_drift_phase_deg)
         << ") Tj\n"
         << "0 -16 Td\n(max_phase_residual_deg: " << formatNumber(info.max_phase_residual_deg)
         << ") Tj\n"
         << "0 -16 Td\n(THRU: " << pdfEscape(thru) << ") Tj\n"
         << "ET\n";
    return body.str();
}

}  // namespace

bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics)
{
    const std::string stream = makeContentStream(info);

    std::ostringstream pdf;
    std::vector<std::size_t> offsets(6, 0);

    auto mark = [&](int obj) {
        offsets[static_cast<std::size_t>(obj)] = static_cast<std::size_t>(pdf.tellp());
    };

    pdf << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    mark(1);
    pdf << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    mark(2);
    pdf << "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    mark(3);
    pdf << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n";
    mark(4);
    pdf << "4 0 obj\n<< /Length " << stream.size() << " >>\nstream\n" << stream
        << "endstream\nendobj\n";
    mark(5);
    pdf << "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    const auto xref_pos = static_cast<std::size_t>(pdf.tellp());
    pdf << "xref\n0 6\n";
    pdf << "0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[static_cast<std::size_t>(i)]);
        pdf << buf;
    }
    pdf << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref_pos << "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        diagnostics = "cannot write pdf: " + path.string();
        return false;
    }
    const auto bytes = pdf.str();
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        diagnostics = "pdf write failed";
        return false;
    }
    return true;
}

bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open pdf";
        return false;
    }
    char head[5]{};
    in.read(head, 5);
    if (!in || std::string(head, 5) != "%PDF-") {
        diagnostics = "not a PDF header";
        return false;
    }
    in.seekg(0, std::ios::end);
    if (in.tellg() <= 0) {
        diagnostics = "empty pdf";
        return false;
    }
    return true;
}

}  // namespace afar::report
