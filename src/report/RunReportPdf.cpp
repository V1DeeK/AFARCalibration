#include "RunReportPdf.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace afar::report {
namespace {

std::string pdfEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
            out.push_back('?');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string makeContentStream(const RunReportInfo& info)
{
    std::ostringstream body;
    body << "BT\n/F1 14 Tf\n50 750 Td\n(AFAR RX Calibration Studio - series report) Tj\n"
         << "0 -24 Td\n/F1 12 Tf\n(run_id: " << pdfEscape(info.run_id) << ") Tj\n"
         << "0 -18 Td\n(software_version: " << pdfEscape(info.software_version) << ") Tj\n"
         << "0 -18 Td\n(completed_states: " << info.completed_states << ") Tj\n"
         << "0 -18 Td\n(valid_direct_count: " << info.valid_direct_count << ") Tj\n"
         << "0 -18 Td\n(series_path: " << pdfEscape(info.series_path) << ") Tj\n"
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
    // Объекты с заранее известными смещениями соберём через xref после.
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
