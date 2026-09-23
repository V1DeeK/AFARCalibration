#pragma once

#include "AttenuatorCodes.h"
#include "RunConfig.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace afar::test {

inline std::filesystem::path writeProbeFixtures(const std::filesystem::path& dir,
                                                const std::string& run_id,
                                                int points = 201)
{
    std::filesystem::create_directories(dir);
    const auto cfg_path = dir / "run-config.json";
    const auto csv_path = dir / "attenuator-codes.csv";

    std::ofstream cfg(cfg_path, std::ios::binary);
    cfg << "{\n"
           "  \"schema\": \"afar.stage1.run-config/v1\",\n"
           "  \"run_id\": \""
        << run_id
        << "\",\n"
           "  \"vna\": {\n"
           "    \"model\": \"PLANAR C2220\",\n"
           "    \"host\": \"127.0.0.1\",\n"
           "    \"port\": 5025,\n"
           "    \"s_parameter\": \"S21\",\n"
           "    \"f_start_hz\": 4900000000,\n"
           "    \"f_stop_hz\": 6000000000,\n"
           "    \"points\": "
        << points
        << ",\n"
           "    \"ifbw_hz\": 1000,\n"
           "    \"power_dbm\": -30.0,\n"
           "    \"averages\": 1\n"
           "  },\n"
           "  \"controller\": { \"driver\": \"sim\", \"endpoint\": \"sim\" },\n"
           "  \"dut\": {\n"
           "    \"serial\": \"PROBE\",\n"
           "    \"channels\": { \"first\": 1, \"last\": 1 },\n"
           "    \"phase_codes\": { \"first\": 0, \"last\": 3, \"lsb_deg\": 5.625 },\n"
           "    \"attenuator_codes_file\": \"attenuator-codes.csv\",\n"
           "    \"reference\": { \"att_code\": 0, \"phase_code\": 0 }\n"
           "  },\n"
           "  \"timing\": { \"settle_ms\": 0, \"reference_after_phase_row\": true },\n"
           "  \"limits\": { \"max_drift_phase_deg\": 1.0, \"max_phase_residual_deg\": 2.8125 }\n"
           "}\n";

    std::ofstream csv(csv_path, std::ios::binary);
    csv << "att_code,att_cmd_db,enabled,settle_ms\n"
           "0,0.00,true,0\n"
           "1,0.50,true,0\n"
           "2,1.00,false,0\n";

    return dir;
}

inline bool loadProbeConfig(const std::filesystem::path& dir,
                            RunConfig& cfg,
                            AttenuatorCodes& att,
                            std::string& diag)
{
    if (!RunConfig::loadFromFile(dir / "run-config.json", cfg, diag)) {
        return false;
    }
    return AttenuatorCodes::loadFromFile(dir / "attenuator-codes.csv", att, diag);
}

}  // namespace afar::test
