#include "SeriesDirectory.h"

#include <stdexcept>
#include <system_error>

namespace afar {
namespace {

bool validateRunId(const std::string& run_id, std::string& diagnostics)
{
    if (run_id.empty()) {
        diagnostics = "run_id is empty";
        return false;
    }
    if (run_id.find("..") != std::string::npos
        || run_id.find('/') != std::string::npos
        || run_id.find('\\') != std::string::npos) {
        diagnostics = "run_id must not contain path separators or '..'";
        return false;
    }
    return true;
}

bool isStrictlyUnder(const std::filesystem::path& child,
                     const std::filesystem::path& parent)
{
    const auto rel = child.lexically_normal().lexically_relative(parent.lexically_normal());
    if (rel.empty() || rel == "." || rel == "..") {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

}  // namespace

bool SeriesDirectory::create(const std::filesystem::path& data_root,
                             const std::string& run_id,
                             const std::filesystem::path& run_config_src,
                             const std::filesystem::path& attenuator_csv_src,
                             SeriesDirectory& out,
                             std::string& diagnostics)
{
    diagnostics.clear();
    if (!validateRunId(run_id, diagnostics)) {
        return false;
    }

    std::error_code ec;
    const auto root_abs = std::filesystem::absolute(data_root, ec).lexically_normal();
    if (ec) {
        diagnostics = "cannot resolve data root: " + ec.message();
        return false;
    }

    const auto series_abs = (root_abs / run_id).lexically_normal();
    if (!isStrictlyUnder(series_abs, root_abs)) {
        diagnostics = "path escape: series must be under data root";
        return false;
    }

    std::filesystem::create_directories(series_abs, ec);
    if (ec) {
        diagnostics = "cannot create series directory: " + ec.message();
        return false;
    }

    const auto cfg_dst = series_abs / kRunConfig;
    const auto csv_dst = series_abs / kAttenuatorCodes;
    std::filesystem::copy_file(run_config_src, cfg_dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        diagnostics = "cannot copy run-config.json: " + ec.message();
        return false;
    }
    std::filesystem::copy_file(attenuator_csv_src, csv_dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        diagnostics = "cannot copy attenuator-codes.csv: " + ec.message();
        return false;
    }

    out.root_ = series_abs;
    out.run_id_ = run_id;
    return true;
}

bool SeriesDirectory::openExisting(const std::filesystem::path& series_path,
                                   SeriesDirectory& out,
                                   std::string& diagnostics)
{
    diagnostics.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(series_path, ec) || ec) {
        diagnostics = "series directory does not exist: " + series_path.string();
        return false;
    }
    const auto abs = std::filesystem::absolute(series_path, ec).lexically_normal();
    if (ec) {
        diagnostics = "cannot resolve series path: " + ec.message();
        return false;
    }
    if (!std::filesystem::exists(abs / kRunConfig)
        || !std::filesystem::exists(abs / kAttenuatorCodes)) {
        diagnostics = "series is missing run-config.json or attenuator-codes.csv";
        return false;
    }
    out.root_ = abs;
    out.run_id_ = abs.filename().string();
    return true;
}

std::filesystem::path SeriesDirectory::joinChecked(const char* filename) const
{
    const auto p = (root_ / filename).lexically_normal();
    if (!isInside(p)) {
        throw std::runtime_error(std::string("path escape forbidden: ") + filename);
    }
    return p;
}

bool SeriesDirectory::isInside(const std::filesystem::path& candidate) const
{
    std::error_code ec;
    const auto base = root_.lexically_normal();
    auto cand = candidate;
    if (!cand.is_absolute()) {
        cand = std::filesystem::absolute(candidate, ec);
        if (ec) {
            return false;
        }
    }
    cand = cand.lexically_normal();
    if (cand == base) {
        return true;
    }
    return isStrictlyUnder(cand, base);
}

std::filesystem::path SeriesDirectory::runConfigPath() const
{
    return joinChecked(kRunConfig);
}
std::filesystem::path SeriesDirectory::attenuatorCodesPath() const
{
    return joinChecked(kAttenuatorCodes);
}
std::filesystem::path SeriesDirectory::rawS21Path() const
{
    return joinChecked(kRawS21);
}
std::filesystem::path SeriesDirectory::runEventsPath() const
{
    return joinChecked(kRunEvents);
}
std::filesystem::path SeriesDirectory::directLutPath() const
{
    return joinChecked(kDirectLut);
}
std::filesystem::path SeriesDirectory::inverseLutPath() const
{
    return joinChecked(kInverseLut);
}
std::filesystem::path SeriesDirectory::reportPath() const
{
    return joinChecked(kReport);
}
std::filesystem::path SeriesDirectory::manifestPath() const
{
    return joinChecked(kManifest);
}

}  // namespace afar
