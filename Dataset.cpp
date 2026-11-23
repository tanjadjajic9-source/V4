#include "Dataset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <numeric>

namespace XboxOTFM {

namespace {

constexpr uint8_t kRecAdd = 1;
constexpr uint8_t kRecRemove = 2;
constexpr uint32_t kMaxRecordPayload = 1u << 20;
constexpr size_t kMaxNameBytes = 120;
constexpr uint32_t kFileVersion = 1;

void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(static_cast<uint8_t>(x)); v.push_back(static_cast<uint8_t>(x >> 8)); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void Put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t Get64(const uint8_t* p) { return static_cast<uint64_t>(Get32(p)) | (static_cast<uint64_t>(Get32(p + 4)) << 32); }

uint32_t RecordCrc(uint8_t type, const uint8_t* payload, size_t len) {
    return Crc32(payload, len, Crc32(&type, 1));
}

std::string CleanName(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) continue;
        out += static_cast<char>(c);
        if (out.size() >= kMaxNameBytes) break;
    }
    // do not cut a UTF-8 sequence in half
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
    return out;
}

bool WriteRecord(FILE* f, uint8_t type, const std::vector<uint8_t>& payload) {
    uint8_t head[9];
    head[0] = type;
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const uint32_t crc = RecordCrc(type, payload.data(), payload.size());
    for (int i = 0; i < 4; ++i) head[1 + i] = static_cast<uint8_t>(len >> (8 * i));
    for (int i = 0; i < 4; ++i) head[5 + i] = static_cast<uint8_t>(crc >> (8 * i));
    if (std::fwrite(head, 1, sizeof(head), f) != sizeof(head)) return false;
    if (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), f) != payload.size()) return false;
    return true;
}

} // namespace

Dataset::Dataset(std::string folder, size_t maxCards)
    : m_folder(std::move(folder))
    , m_maxCards(std::max<size_t>(1, maxCards)) {
}

Dataset::~Dataset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    CloseFileLocked();
}

std::string Dataset::FilePath() const {
    return m_folder + "/cards.otds";
}

std::string Dataset::TempFilePath() const {
    return m_folder + "/cards.otds.new";
}

void Dataset::CloseFileLocked() {
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

bool Dataset::WriteHeader(FILE* f) const {
    uint8_t head[8] = { 'O', 'T', 'D', 'S', 0, 0, 0, 0 };
    for (int i = 0; i < 4; ++i) head[4 + i] = static_cast<uint8_t>(kFileVersion >> (8 * i));
    return std::fwrite(head, 1, sizeof(head), f) == sizeof(head);
}

bool Dataset::Open(std::vector<std::string>* warnings) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return OpenLocked(warnings);
}

bool Dataset::OpenLocked(std::vector<std::string>* warnings) {
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };
    CloseFileLocked();
    m_cards.clear();
    m_hashToId.clear();
    m_fileBytes = 0;
    m_fileRecords = 0;
    m_deadRecords = 0;
    m_diskBroken = false;
    m_orderDirty = true;
    if (m_folder.empty()) return true;

    std::error_code ec;
    std::filesystem::create_directories(m_folder, ec);
    if (ec) {
        warn("The picture folder could not be created (" + ec.message() + "): pictures will not be kept after the app closes.");
        m_diskBroken = true;
        return true;
    }

    // A crash in the middle of a compaction can leave only the new file behind.
    if (!std::filesystem::exists(FilePath(), ec) && std::filesystem::exists(TempFilePath(), ec)) {
        std::filesystem::rename(TempFilePath(), FilePath(), ec);
    }

    uint64_t validBytes = 0;
    bool needFreshFile = true;
    bool trimTail = false;
    if (std::filesystem::exists(FilePath(), ec)) {
        FILE* f = std::fopen(FilePath().c_str(), "rb");
        if (!f) {
            warn("The saved pictures could not be opened; starting with an empty set.");
            m_diskBroken = true;
            return true;
        }
        uint8_t head[8] = {};
        const bool headOk = std::fread(head, 1, sizeof(head), f) == sizeof(head) && std::memcmp(head, "OTDS", 4) == 0 && Get32(head + 4) == kFileVersion;
        if (!headOk) {
            std::fclose(f);
            warn("The saved picture file was not recognised; it was set aside and a new one started.");
            std::filesystem::rename(FilePath(), FilePath() + ".bad", ec);
        } else {
            needFreshFile = false;
            validBytes = sizeof(head);
            std::vector<uint8_t> payload;
            for (;;) {
                uint8_t rh[9];
                const size_t got = std::fread(rh, 1, sizeof(rh), f);
                if (got == 0) break;
                if (got != sizeof(rh)) { warn("The end of the saved picture file was cut off; the damaged part was ignored."); break; }
                const uint8_t type = rh[0];
                const uint32_t len = Get32(rh + 1);
                const uint32_t crc = Get32(rh + 5);
                if ((type != kRecAdd && type != kRecRemove) || len > kMaxRecordPayload) { warn("Part of the saved picture file was damaged and was ignored."); break; }
                payload.resize(len);
                if (len > 0 && std::fread(payload.data(), 1, len, f) != len) { warn("The end of the saved picture file was cut off; the damaged part was ignored."); break; }
                if (RecordCrc(type, payload.data(), len) != crc) { warn("Part of the saved picture file was damaged and was ignored."); break; }
                if (type == kRecAdd) {
                    if (len < 8 + 8 + 4 + 4 + 2) { warn("A saved picture record was malformed and was ignored."); break; }
                    Card c;
                    c.id = Get64(payload.data());
                    c.hash = Get64(payload.data() + 8);
                    const uint32_t w = Get32(payload.data() + 16);
                    const uint32_t h = Get32(payload.data() + 20);
                    const uint16_t nameLen = Get16(payload.data() + 24);
                    const size_t need = 26 + static_cast<size_t>(nameLen) + static_cast<size_t>(kCardSize) * kCardSize * 3;
                    if (w != kCardSize || h != kCardSize || need != len) { warn("A saved picture record was malformed and was ignored."); break; }
                    c.name.assign(reinterpret_cast<const char*>(payload.data() + 26), nameLen);
                    c.rgb.assign(payload.data() + 26 + nameLen, payload.data() + len);
                    if (m_cards.size() < m_maxCards && m_hashToId.find(c.hash) == m_hashToId.end()) {
                        m_nextId = std::max(m_nextId, c.id + 1);
                        IndexCardLocked(c);
                        m_cards.push_back(std::move(c));
                    } else {
                        m_nextId = std::max(m_nextId, c.id + 1);
                        ++m_deadRecords;
                    }
                } else {
                    if (len != 8) { warn("A saved picture record was malformed and was ignored."); break; }
                    const uint64_t id = Get64(payload.data());
                    const auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](const Card& c) { return c.id == id; });
                    if (it != m_cards.end()) {
                        const auto hit = m_hashToId.find(it->hash);
                        if (hit != m_hashToId.end() && hit->second == id) m_hashToId.erase(hit);
                        m_cards.erase(it);
                    }
                    m_deadRecords += 2; // the add record it cancels + the remove record itself
                }
                ++m_fileRecords;
                validBytes += sizeof(rh) + len;
            }
            std::fclose(f);
            // Anything after the last good record is damaged. It is removed below by writing the good
            // pictures into a fresh file (std::filesystem::resize_file is not used because it is not
            // available in every Windows app build).
            const uint64_t onDisk = std::filesystem::file_size(FilePath(), ec);
            if (!ec && onDisk > validBytes) trimTail = true;
        }
    }

    if (needFreshFile) {
        FILE* f = std::fopen(FilePath().c_str(), "wb");
        if (!f || !WriteHeader(f)) {
            if (f) std::fclose(f);
            warn("The picture file could not be created: pictures will not be kept after the app closes.");
            m_diskBroken = true;
            return true;
        }
        std::fclose(f);
        validBytes = 8;
    }

    m_fileBytes = validBytes;
    if (trimTail && !m_diskBroken) {
        if (!RewriteLocked()) {
            warn("The damaged end of the picture file could not be removed: new pictures will not be kept after the app closes.");
            m_diskBroken = true;
        }
    }
    if (!m_diskBroken && !m_file && !ReopenForAppendLocked()) {
        warn("The picture file could not be opened for writing: new pictures will not be kept after the app closes.");
    }
    MaybeCompactLocked();
    ++m_version;
    return true;
}

bool Dataset::ReopenForAppendLocked() {
    CloseFileLocked();
    m_file = std::fopen(FilePath().c_str(), "ab");
    if (!m_file) {
        m_diskBroken = true;
        return false;
    }
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(FilePath(), ec);
    if (!ec) m_fileBytes = size;
    return true;
}

void Dataset::IndexCardLocked(const Card& c) {
    m_hashToId[c.hash] = c.id;
}

void Dataset::RebuildIndexLocked() {
    m_hashToId.clear();
    for (const Card& c : m_cards) m_hashToId[c.hash] = c.id;
}

bool Dataset::AppendAddRecord(const Card& c) {
    if (m_diskBroken || !m_file) return false;
    std::vector<uint8_t> p;
    p.reserve(26 + c.name.size() + c.rgb.size());
    Put64(p, c.id);
    Put64(p, c.hash);
    Put32(p, kCardSize);
    Put32(p, kCardSize);
    Put16(p, static_cast<uint16_t>(c.name.size()));
    p.insert(p.end(), c.name.begin(), c.name.end());
    p.insert(p.end(), c.rgb.begin(), c.rgb.end());
    if (!WriteRecord(m_file, kRecAdd, p) || std::fflush(m_file) != 0) {
        m_diskBroken = true;
        return false;
    }
    m_fileBytes += 9 + p.size();
    ++m_fileRecords;
    return true;
}

bool Dataset::AppendRemoveRecord(uint64_t id) {
    if (m_diskBroken || !m_file) return false;
    std::vector<uint8_t> p;
    Put64(p, id);
    if (!WriteRecord(m_file, kRecRemove, p) || std::fflush(m_file) != 0) {
        m_diskBroken = true;
        return false;
    }
    m_fileBytes += 9 + p.size();
    ++m_fileRecords;
    m_deadRecords += 2;
    return true;
}

bool Dataset::RewriteLocked() {
    if (m_folder.empty() || m_diskBroken) return false;
    CloseFileLocked();
    FILE* f = std::fopen(TempFilePath().c_str(), "wb");
    if (!f) { ReopenForAppendLocked(); return false; }
    bool ok = WriteHeader(f);
    uint64_t bytes = 8;
    for (const Card& c : m_cards) {
        if (!ok) break;
        std::vector<uint8_t> p;
        Put64(p, c.id);
        Put64(p, c.hash);
        Put32(p, kCardSize);
        Put32(p, kCardSize);
        Put16(p, static_cast<uint16_t>(c.name.size()));
        p.insert(p.end(), c.name.begin(), c.name.end());
        p.insert(p.end(), c.rgb.begin(), c.rgb.end());
        ok = WriteRecord(f, kRecAdd, p);
        bytes += 9 + p.size();
    }
    ok = (std::fflush(f) == 0) && ok;
    std::fclose(f);
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(TempFilePath(), FilePath(), ec);
        if (ec) {
            std::filesystem::remove(FilePath(), ec);
            ec.clear();
            std::filesystem::rename(TempFilePath(), FilePath(), ec);
            ok = !ec;
        }
    }
    if (!ok) {
        std::filesystem::remove(TempFilePath(), ec);
        ReopenForAppendLocked();
        return false;
    }
    m_fileBytes = bytes;
    m_fileRecords = m_cards.size();
    m_deadRecords = 0;
    return ReopenForAppendLocked();
}

void Dataset::MaybeCompactLocked() {
    if (m_diskBroken || m_folder.empty()) return;
    if (m_deadRecords >= 32 && m_deadRecords * 3 >= m_fileRecords) RewriteLocked();
}

Dataset::AddResult Dataset::Add(const uint8_t* rgb, uint32_t w, uint32_t h, const std::string& name) {
    AddResult res;
    if (!rgb || w < 8 || h < 8 || w > 8192 || h > 8192) {
        res.status = AddStatus::Invalid;
        res.message = "The picture size is not usable (it must be between 8 and 8192 pixels).";
        return res;
    }
    Card card;
    if (w == kCardSize && h == kCardSize) {
        card.rgb.assign(rgb, rgb + static_cast<size_t>(w) * h * 3);
    } else {
        Rgb8Image fitted = FitToSquare(rgb, w, h, kCardSize, FitMode::CropCenter);
        if (!fitted.Valid()) {
            res.status = AddStatus::Invalid;
            res.message = "The picture could not be resized.";
            return res;
        }
        card.rgb = std::move(fitted.px);
    }
    card.hash = Fnv1a64(card.rgb.data(), card.rgb.size());
    card.name = CleanName(name);

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto dup = m_hashToId.find(card.hash);
    if (dup != m_hashToId.end()) {
        const auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](const Card& c) { return c.id == dup->second; });
        if (it != m_cards.end() && it->rgb == card.rgb) {
            res.status = AddStatus::Duplicate;
            res.id = it->id;
            res.message = "This picture is already in the set.";
            return res;
        }
    }
    if (m_cards.size() >= m_maxCards) {
        res.status = AddStatus::Full;
        res.message = "The set is full (" + std::to_string(m_maxCards) + " pictures). Remove some pictures first.";
        return res;
    }
    card.id = m_nextId++;
    const bool saved = AppendAddRecord(card);
    res.status = AddStatus::Added;
    res.id = card.id;
    if (!saved && !m_folder.empty()) res.message = "Added, but it could not be saved to the Xbox's storage.";
    IndexCardLocked(card);
    m_cards.push_back(std::move(card));
    m_orderDirty = true;
    ++m_version;
    return res;
}

bool Dataset::Remove(uint64_t id) {
    std::vector<uint64_t> ids(1, id);
    return RemoveMany(ids) == 1;
}

size_t Dataset::RemoveMany(const std::vector<uint64_t>& ids) {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t removed = 0;
    for (uint64_t id : ids) {
        const auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](const Card& c) { return c.id == id; });
        if (it == m_cards.end()) continue;
        const auto hit = m_hashToId.find(it->hash);
        if (hit != m_hashToId.end() && hit->second == id) m_hashToId.erase(hit);
        m_cards.erase(it);
        AppendRemoveRecord(id);
        ++removed;
    }
    if (removed > 0) {
        m_orderDirty = true;
        ++m_version;
        MaybeCompactLocked();
    }
    return removed;
}

void Dataset::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cards.empty()) return;
    m_cards.clear();
    m_hashToId.clear();
    m_orderDirty = true;
    m_cursor = 0;
    ++m_version;
    if (!m_folder.empty()) {
        CloseFileLocked();
        FILE* f = std::fopen(FilePath().c_str(), "wb");
        bool ok = f && WriteHeader(f) && std::fflush(f) == 0;
        if (f) std::fclose(f);
        m_fileBytes = 8;
        m_fileRecords = 0;
        m_deadRecords = 0;
        m_diskBroken = !ok;
        if (ok) ReopenForAppendLocked();
    }
}

size_t Dataset::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cards.size();
}

uint64_t Dataset::Version() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_version;
}

uint64_t Dataset::DiskBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fileBytes;
}

std::vector<DatasetCardInfo> Dataset::List(size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DatasetCardInfo> out;
    for (size_t i = offset; i < m_cards.size() && out.size() < limit; ++i) {
        DatasetCardInfo info;
        info.id = m_cards[i].id;
        info.hash = m_cards[i].hash;
        info.w = kCardSize;
        info.h = kCardSize;
        info.name = m_cards[i].name;
        out.push_back(std::move(info));
    }
    return out;
}

bool Dataset::GetImage(uint64_t id, Rgb8Image& out, std::string* name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Card& c : m_cards) {
        if (c.id == id) {
            out.w = kCardSize;
            out.h = kCardSize;
            out.px = c.rgb;
            if (name) *name = c.name;
            return true;
        }
    }
    return false;
}

double Dataset::EpochProgress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const double frac = m_order.empty() ? 0.0 : static_cast<double>(m_cursor) / static_cast<double>(m_order.size());
    return static_cast<double>(m_epochs) + std::min(1.0, frac);
}

void Dataset::ResetEpochCounter() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_epochs = 0;
    m_cursor = 0;
    m_orderDirty = true;
}

bool Dataset::SampleBatch(uint32_t count, uint32_t res, const AugmentOptions& aug, std::mt19937_64& rng, std::vector<float>& outCHW) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outCHW.clear();
    if (m_cards.empty() || count == 0) return false;
    res = std::clamp<uint32_t>(res, 8u, 128u);
    const size_t hw = static_cast<size_t>(res) * res;
    outCHW.assign(static_cast<size_t>(count) * 3 * hw, 0.0f);

    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<float> tmp;
    for (uint32_t n = 0; n < count; ++n) {
        if (m_orderDirty || m_order.size() != m_cards.size()) {
            m_order.resize(m_cards.size());
            std::iota(m_order.begin(), m_order.end(), 0u);
            std::shuffle(m_order.begin(), m_order.end(), rng);
            m_cursor = 0;
            m_orderDirty = false;
        }
        if (m_cursor >= m_order.size()) {
            std::shuffle(m_order.begin(), m_order.end(), rng);
            m_cursor = 0;
            ++m_epochs;
        }
        const Card& card = m_cards[m_order[m_cursor++]];

        double cx = 0.0, cy = 0.0, side = kCardSize;
        if (aug.zoom) {
            const double scale = 0.85 + 0.15 * unit(rng);
            side = kCardSize * scale;
            cx = (kCardSize - side) * unit(rng);
            cy = (kCardSize - side) * unit(rng);
        }
        const bool flip = aug.flip && unit(rng) < 0.5;
        float contrast = 1.0f, bright = 0.0f;
        if (aug.color) {
            contrast = static_cast<float>(0.92 + 0.16 * unit(rng));
            bright = static_cast<float>(-0.06 + 0.12 * unit(rng));
        }
        if (!ResampleRegion(card.rgb.data(), kCardSize, kCardSize, cx, cy, side, side, res, res, flip, tmp)) {
            continue; // cannot happen with valid sizes; leaves a mid-grey sample rather than failing the whole batch
        }
        float* dst = outCHW.data() + static_cast<size_t>(n) * 3 * hw;
        for (size_t i = 0; i < hw; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                float v = tmp[i * 3 + c] / 127.5f - 1.0f;
                v = std::clamp(v * contrast + bright, -1.0f, 1.0f);
                dst[c * hw + i] = v;
            }
        }
    }
    return true;
}

} // namespace XboxOTFM
