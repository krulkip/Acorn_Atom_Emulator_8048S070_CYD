#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include "atommc_stub.h"

extern "C" uint8_t atom_serial_show_atommc_comments;

// V121 AtoMMC2 V3.02E controller emulation.
//
// V121 completes the demo-oriented EXEC + path/wildcard behaviour:
//   *EXEC uses the genuine ATMMC3E ROM's OSRDCH redirection and the existing
//         FILE_OPEN_READ / FILE_GETINFO / READ_BYTES controller interface.
//   *CAT/*INFO directory iteration now exposes ordinary files as well as ATM
//         files, allowing text files used by *EXEC to be visible.
//   absolute '/', relative '.', '..', '/' and '\\' paths are canonicalised
//         against the emulated AtoMMC root.
//   DOS-style '*' and '?' wildcards continue to be matched case-insensitively.
//
// The SD backend is rooted at /ATOM/TAPES. Existing .atm files are exposed
// to AtoMMC without their .atm suffix. Other files retain their SD filename.
// Logical Atom files written through AtoMMC are stored as .atm files.

static const char *MMC_ROOT = "/ATOM/TAPES";

static uint8_t mmc_cmd_value = 0x3F;
static uint8_t mmc_latch = 0x00;
static uint8_t mmc_cfg = 0xFF;
static uint8_t mmc_heartbeat = 0x55;
static uint32_t mmc_access_count = 0;

// AtoMMC PIC globalData buffer semantics.
// The real AtoMMC PIC protocol uses one shared globalData buffer.
// PORT4 mirrors that here instead of keeping separate 512-byte read/write
// arrays, saving 512 bytes of scarce CYD internal DRAM without reducing
// the protocol buffer size. Read and write phases are sequential.
static uint8_t mmc_data_buf[512];
static size_t  mmc_write_len = 0;
static size_t  mmc_read_len = 0;
static size_t  mmc_read_pos = 0;
static bool    mmc_accept_write_data = false;

static fs::File mmc_file;
static fs::File mmc_dir;
static String   mmc_cwd = "";       // relative to MMC_ROOT, no leading slash
static String   mmc_dir_pattern = "*";

static void log_access(const char *op, uint16_t addr, uint8_t value)
{
    // Keep detailed startup/protocol diagnostics bounded.
    if (mmc_access_count < 180) {
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] %s $%04X = $%02X\n", op, addr, value);
    } else if (mmc_access_count == 180) {
        if (atom_serial_show_atommc_comments) Serial.println("[ATOMMC] further register diagnostics suppressed");
    }
    ++mmc_access_count;
}

static void set_read_data(const uint8_t *src, size_t len)
{
    if (len > sizeof(mmc_data_buf)) len = sizeof(mmc_data_buf);
    if (len && src) memcpy(mmc_data_buf, src, len);
    mmc_read_len = len;
    mmc_read_pos = 0;
}

static void set_read_string(const String &s, uint8_t attrib)
{
    size_t n = s.length();
    if (n > sizeof(mmc_data_buf) - 2) n = sizeof(mmc_data_buf) - 2;
    memcpy(mmc_data_buf, s.c_str(), n);
    mmc_data_buf[n++] = 0;
    mmc_data_buf[n++] = attrib;
    mmc_read_len = n;
    mmc_read_pos = 0;
}

static String uploaded_string()
{
    size_t n = 0;
    while (n < mmc_write_len && mmc_data_buf[n]) ++n;
    String s;
    for (size_t i = 0; i < n; ++i) s += (char)mmc_data_buf[i];
    return s;
}

static bool wildcard_match_ci(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            ++pat;
            if (!*pat) return true;
            while (*str) {
                if (wildcard_match_ci(pat, str)) return true;
                ++str;
            }
            return wildcard_match_ci(pat, str);
        }
        if (!*str) return false;
        if (*pat != '?') {
            char a = *pat, b = *str;
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) return false;
        }
        ++pat;
        ++str;
    }
    return *str == 0;
}

static String normalize_relative(String rel, bool directoryTarget)
{
    (void)directoryTarget;
    rel.replace('\\', '/');
    rel.trim();

    // A leading slash is an AtoMMC absolute path.  It is rooted at
    // /ATOM/TAPES, not at the ESP32 SD-card root.
    bool absolute = rel.startsWith("/");
    while (rel.startsWith("/")) rel.remove(0, 1);

    String base;
    if (!rel.length()) {
        base = absolute ? String("") : mmc_cwd;
    } else if (absolute) {
        base = rel;
    } else {
        base = mmc_cwd.length() ? mmc_cwd + "/" + rel : rel;
    }

    // Canonicalize '.', '..' and duplicate separators, clamped at MMC_ROOT.
    String parts[24];
    int count = 0;
    int pos = 0;
    while (pos <= (int)base.length()) {
        int slash = base.indexOf('/', pos);
        if (slash < 0) slash = base.length();
        String part = base.substring(pos, slash);
        if (part.length() && part != ".") {
            if (part == "..") {
                if (count > 0) --count;
            } else if (count < 24) {
                parts[count++] = part;
            }
        }
        if (slash >= (int)base.length()) break;
        pos = slash + 1;
    }

    String out;
    for (int i = 0; i < count; ++i) {
        if (out.length()) out += "/";
        out += parts[i];
    }
    return out;
}

static String full_sd_path_from_rel(const String &rel)
{
    if (!rel.length()) return String(MMC_ROOT);
    return String(MMC_ROOT) + "/" + rel;
}

static bool open_atom_file(String requested)
{
    requested = normalize_relative(requested, false);
    String p = full_sd_path_from_rel(requested);

    if (mmc_file) mmc_file.close();

    mmc_file = SD.open(p.c_str(), FILE_READ);
    if (mmc_file && !mmc_file.isDirectory()) {
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] OPEN %s\n", p.c_str());
        return true;
    }
    if (mmc_file) mmc_file.close();

    // Current emulator media convention: logical Atom name -> name.atm
    if (!requested.endsWith(".atm") && !requested.endsWith(".ATM")) {
        String pa = p + ".atm";
        mmc_file = SD.open(pa.c_str(), FILE_READ);
        if (mmc_file && !mmc_file.isDirectory()) {
            if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] OPEN %s (logical %s)\n",
                          pa.c_str(), requested.c_str());
            return true;
        }
        if (mmc_file) mmc_file.close();
    }
    return false;
}

static String display_name_for_file(String name)
{
    if (name.endsWith(".atm") || name.endsWith(".ATM")) {
        name.remove(name.length() - 4);
    }
    return name;
}

static void split_dir_and_pattern(String spec, String &dirRel, String &pattern)
{
    spec.replace('\\', '/');
    spec.trim();

    bool hasWild = spec.indexOf('*') >= 0 || spec.indexOf('?') >= 0;

    if (!spec.length()) {
        dirRel = mmc_cwd;
        pattern = "*";
        return;
    }

    if (hasWild) {
        int slash = spec.lastIndexOf('/');
        if (slash >= 0) {
            pattern = spec.substring(slash + 1);
            String d = spec.substring(0, slash);
            dirRel = normalize_relative(d, true);
        } else {
            pattern = spec;
            dirRel = mmc_cwd;
        }
    } else {
        // Firmware treats a non-wildcard argument to DIR_OPEN as directory.
        dirRel = normalize_relative(spec, true);
        pattern = "*";
    }
    if (!pattern.length()) pattern = "*";
}

static uint8_t fat_not_found() { return 0x44; } // COMPLETE | FR_NO_FILE
static uint8_t fat_no_path()   { return 0x45; } // COMPLETE | FR_NO_PATH
static uint8_t fat_denied()    { return 0x47; } // COMPLETE | FR_DENIED
static uint8_t fat_exists()    { return 0x48; } // COMPLETE | FR_EXIST

static String logical_file_rel(String requested)
{
    String rel = normalize_relative(requested, false);
    if (!rel.endsWith(".atm") && !rel.endsWith(".ATM")) rel += ".atm";
    return rel;
}

static String existing_object_rel(String requested, bool &isDir)
{
    String rel = normalize_relative(requested, false);
    String p = full_sd_path_from_rel(rel);
    fs::File f = SD.open(p.c_str());
    if (f) {
        isDir = f.isDirectory();
        f.close();
        return rel;
    }
    if (!rel.endsWith(".atm") && !rel.endsWith(".ATM")) {
        String ar = rel + ".atm";
        p = full_sd_path_from_rel(ar);
        f = SD.open(p.c_str());
        if (f) {
            isDir = f.isDirectory();
            f.close();
            return ar;
        }
    }
    isDir = false;
    return String();
}

static String uploaded_string_at(size_t &pos)
{
    String s;
    while (pos < mmc_write_len && mmc_data_buf[pos])
        s += (char)mmc_data_buf[pos++];
    if (pos < mmc_write_len && mmc_data_buf[pos] == 0) ++pos;
    return s;
}

static void cmd_dir_open()
{
    String spec = uploaded_string();
    String dirRel, pattern;
    split_dir_and_pattern(spec, dirRel, pattern);

    if (mmc_dir) mmc_dir.close();
    String p = full_sd_path_from_rel(dirRel);
    mmc_dir = SD.open(p.c_str());

    if (!mmc_dir || !mmc_dir.isDirectory()) {
        if (mmc_dir) mmc_dir.close();
        mmc_cmd_value = fat_no_path();
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] DIR OPEN failed: %s\n", p.c_str());
        return;
    }

    mmc_dir_pattern = pattern;
    mmc_cmd_value = 0x3F; // STATUS_OK
    if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] DIR OPEN %s pattern=%s\n",
                  p.c_str(), pattern.c_str());
}

static void cmd_dir_read()
{
    if (!mmc_dir) {
        mmc_cmd_value = 0x40; // complete/end
        return;
    }

    for (;;) {
        fs::File entry = mmc_dir.openNextFile();
        if (!entry) {
            mmc_cmd_value = 0x40; // STATUS_COMPLETE: no more entries
            set_read_data(nullptr, 0);
            return;
        }

        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);

        bool isDir = entry.isDirectory();
        String logical = isDir ? name : display_name_for_file(name);

        // ATM containers are shown using their logical Atom filename
        // (without .atm). Other files retain their real filename. This is
        // required for genuine *EXEC of ANSI text files such as DEMO.TXT.
        if (!wildcard_match_ci(mmc_dir_pattern.c_str(), logical.c_str())) {
            entry.close();
            continue;
        }

        uint8_t attr = isDir ? 0x10 : 0x00;
        String shown = isDir ? String("<") + logical + ">" : logical;
        set_read_string(shown, attr);
        entry.close();

        mmc_cmd_value = 0x3F; // STATUS_OK
        return;
    }
}

static void cmd_cwd()
{
    String requested = uploaded_string();
    String rel = normalize_relative(requested, true);
    String p = full_sd_path_from_rel(rel);
    fs::File d = SD.open(p.c_str());
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        mmc_cmd_value = fat_no_path();
        return;
    }
    d.close();
    mmc_cwd = rel;
    mmc_cmd_value = 0x40; // STATUS_COMPLETE | FR_OK
    if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] CWD /%s\n", mmc_cwd.c_str());
}

static void cmd_open_read()
{
    String name = uploaded_string();
    if (open_atom_file(name))
        mmc_cmd_value = 0x40; // STATUS_COMPLETE | FR_OK
    else
        mmc_cmd_value = fat_not_found();
}

static void cmd_get_info()
{
    uint8_t info[13] = {0};
    if (!mmc_file) {
        mmc_cmd_value = fat_not_found();
        return;
    }
    uint32_t size = (uint32_t)mmc_file.size();
    info[0] = (uint8_t)size;
    info[1] = (uint8_t)(size >> 8);
    info[2] = (uint8_t)(size >> 16);
    info[3] = (uint8_t)(size >> 24);
    // bytes 4..11 are FAT cluster/fptr details; not needed by Step 3.
    info[12] = 0x00;
    set_read_data(info, sizeof(info));
    mmc_cmd_value = 0x3F; // STATUS_OK
}

static void cmd_read_bytes()
{
    if (!mmc_file) {
        mmc_cmd_value = fat_not_found();
        set_read_data(nullptr, 0);
        return;
    }

    size_t amount = mmc_latch ? mmc_latch : 256;
    if (amount > sizeof(mmc_data_buf)) amount = sizeof(mmc_data_buf);

    size_t got = mmc_file.read(mmc_data_buf, amount);
    mmc_read_len = got;
    mmc_read_pos = 0;

    // Scratch-file semantics used by LOAD/INFO: a short final read is not
    // treated as an error; the ROM requests the exact ATM-derived length.
    mmc_cmd_value = 0x40; // STATUS_COMPLETE | FR_OK
}

static void cmd_get_cwd()
{
    String shown = "/";
    if (mmc_cwd.length()) shown += mmc_cwd;
    size_t n = shown.length();
    if (n > sizeof(mmc_data_buf) - 1) n = sizeof(mmc_data_buf) - 1;
    memcpy(mmc_data_buf, shown.c_str(), n);
    mmc_data_buf[n++] = 0;
    mmc_read_len = n;
    mmc_read_pos = 0;
    mmc_cmd_value = 0x3F;
}

static void cmd_open_write()
{
    String requested = uploaded_string();
    String rel = logical_file_rel(requested);
    String p = full_sd_path_from_rel(rel);

    if (mmc_file) mmc_file.close();

    // Match FatFS FA_CREATE_NEW: do not silently overwrite. The AtoMMC ROM
    // handles FR_EXIST and performs its own overwrite confirmation/delete.
    if (SD.exists(p.c_str())) {
        mmc_cmd_value = fat_exists();
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] OPEN WRITE exists: %s\n", p.c_str());
        return;
    }

    mmc_file = SD.open(p.c_str(), FILE_WRITE);
    if (!mmc_file || mmc_file.isDirectory()) {
        if (mmc_file) mmc_file.close();
        mmc_cmd_value = fat_denied();
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] OPEN WRITE failed: %s\n", p.c_str());
        return;
    }

    mmc_cmd_value = 0x40;
    if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] OPEN WRITE %s\n", p.c_str());
}

static void cmd_write_bytes()
{
    if (!mmc_file) {
        mmc_cmd_value = fat_not_found();
        return;
    }

    size_t amount = mmc_latch ? mmc_latch : 256;
    if (amount > mmc_write_len) {
        mmc_cmd_value = 0x48; // COMPLETE | ERROR_NO_DATA
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] WRITE wanted %u, received %u\n",
                      (unsigned)amount, (unsigned)mmc_write_len);
        return;
    }

    size_t written = mmc_file.write(mmc_data_buf, amount);
    if (written != amount) {
        mmc_cmd_value = fat_denied();
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] WRITE short %u/%u\n",
                      (unsigned)written, (unsigned)amount);
        return;
    }
    mmc_file.flush();
    mmc_cmd_value = 0x40;
}

static void cmd_delete()
{
    String requested = uploaded_string();
    bool isDir = false;
    String rel = existing_object_rel(requested, isDir);
    if (!rel.length()) { mmc_cmd_value = fat_not_found(); return; }
    if (isDir) { mmc_cmd_value = fat_denied(); return; }

    String p = full_sd_path_from_rel(rel);
    if (mmc_file) mmc_file.close();
    if (SD.remove(p.c_str())) {
        mmc_cmd_value = 0x40;
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] DELETE %s\n", p.c_str());
    } else {
        mmc_cmd_value = fat_denied();
    }
}

static void cmd_mkdir()
{
    String requested = uploaded_string();
    String rel = normalize_relative(requested, true);
    if (!rel.length()) { mmc_cmd_value = fat_exists(); return; }
    String p = full_sd_path_from_rel(rel);
    if (SD.exists(p.c_str())) { mmc_cmd_value = fat_exists(); return; }
    if (SD.mkdir(p.c_str())) {
        mmc_cmd_value = 0x40;
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] MKDIR %s\n", p.c_str());
    } else mmc_cmd_value = fat_denied();
}

static void cmd_rmdir()
{
    String requested = uploaded_string();
    String rel = normalize_relative(requested, true);
    if (!rel.length()) { mmc_cmd_value = fat_denied(); return; }
    String p = full_sd_path_from_rel(rel);
    fs::File d = SD.open(p.c_str());
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        mmc_cmd_value = fat_no_path();
        return;
    }
    d.close();
    if (SD.rmdir(p.c_str())) {
        mmc_cmd_value = 0x40;
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] RMDIR %s\n", p.c_str());
    } else mmc_cmd_value = fat_denied();
}

static void cmd_rename()
{
    // AtoMMC places two ASCIIZ paths in globalData: source followed by target.
    size_t pos = 0;
    String fromReq = uploaded_string_at(pos);
    String toReq   = uploaded_string_at(pos);
    if (!fromReq.length() || !toReq.length()) {
        mmc_cmd_value = 0x48;
        return;
    }

    bool sourceIsDir = false;
    String fromRel = existing_object_rel(fromReq, sourceIsDir);
    if (!fromRel.length()) { mmc_cmd_value = fat_not_found(); return; }

    String toRel = normalize_relative(toReq, sourceIsDir);
    if (!sourceIsDir && !toRel.endsWith(".atm") && !toRel.endsWith(".ATM"))
        toRel += ".atm";

    String fromP = full_sd_path_from_rel(fromRel);
    String toP   = full_sd_path_from_rel(toRel);
    if (SD.exists(toP.c_str())) { mmc_cmd_value = fat_exists(); return; }

    if (mmc_file) mmc_file.close();
    if (mmc_dir) mmc_dir.close();
    if (SD.rename(fromP.c_str(), toP.c_str())) {
        mmc_cmd_value = 0x40;
        if (atom_serial_show_atommc_comments) Serial.printf("[ATOMMC] REN %s -> %s\n", fromP.c_str(), toP.c_str());
    } else mmc_cmd_value = fat_denied();
}

void atommmc_stub_reset(void)
{
    if (mmc_file) mmc_file.close();
    if (mmc_dir) mmc_dir.close();

    mmc_cmd_value = 0x3F;
    mmc_latch = 0x00;
    mmc_cfg = 0xFF;
    mmc_heartbeat = 0x55;
    mmc_access_count = 0;
    mmc_write_len = 0;
    mmc_read_len = mmc_read_pos = 0;
    mmc_accept_write_data = false;
    mmc_cwd = "";
    mmc_dir_pattern = "*";

    if (atom_serial_show_atommc_comments) Serial.println("[ATOMMC] EXEC + wildcard/path filesystem reset");
}

uint8_t atommmc_stub_read(uint16_t addr)
{
    uint8_t value = 0xFF;

    switch (addr & 0x000F) {
        case 0x00:
            value = mmc_cmd_value;
            break;
        case 0x01:
            value = mmc_latch;
            break;
        case 0x02:
            if (mmc_read_pos < mmc_read_len)
                value = mmc_data_buf[mmc_read_pos++];
            else
                value = 0x00;
            break;
        case 0x03:
            value = 0x00;
            break;
        case 0x04:
            // PIC build does not require AVR-style status handshaking.
            value = 0x00;
            break;
        default:
            value = 0xFF;
            break;
    }

    log_access("RD", addr, value);
    return value;
}

void atommmc_stub_write(uint16_t addr, uint8_t value)
{
    log_access("WR", addr, value);

    switch (addr & 0x000F) {
        case 0x00: // command register
            mmc_accept_write_data = false;

            switch (value) {
                // Directory / navigation
                case 0x00: cmd_dir_open(); break;       // CMD_DIR_OPEN
                case 0x01: cmd_dir_read(); break;       // CMD_DIR_READ
                case 0x02: cmd_cwd(); break;            // CMD_DIR_CWD
                case 0x03: cmd_get_cwd(); break;        // CMD_DIR_GETCWD
                case 0x04: cmd_mkdir(); break;          // CMD_DIR_MKDIR
                case 0x05: cmd_rmdir(); break;          // CMD_DIR_RMDIR
                case 0x08: cmd_rename(); break;         // CMD_RENAME

                // File commands - read-only subset
                case 0x10:                              // CMD_FILE_CLOSE
                    if (mmc_file) mmc_file.close();
                    mmc_cmd_value = 0x40;
                    break;
                case 0x11: cmd_open_read(); break;      // CMD_FILE_OPEN_READ
                case 0x13: cmd_open_write(); break;     // CMD_FILE_OPEN_WRITE
                case 0x14: cmd_delete(); break;         // CMD_FILE_DELETE
                case 0x15: cmd_get_info(); break;       // CMD_FILE_GETINFO

                // Data transfer
                case 0x20:                              // CMD_INIT_READ
                    mmc_read_pos = 0;
                    mmc_cmd_value = 0x3F;
                    break;
                case 0x21:                              // CMD_INIT_WRITE
                    mmc_write_len = 0;
                    mmc_accept_write_data = true;
                    mmc_cmd_value = 0x3F;
                    break;
                case 0x22: cmd_read_bytes(); break;     // CMD_READ_BYTES
                case 0x23: cmd_write_bytes(); break;    // CMD_WRITE_BYTES

                // Startup/utility handshake
                case 0x80: mmc_cmd_value = 0x04; break; // SDHC
                case 0xE0: mmc_cmd_value = 0x30; break; // controller FW 3.0
                case 0xE1: mmc_cmd_value = 0x29; break;
                case 0xF0: mmc_cmd_value = mmc_cfg; break;
                case 0xF1:
                    mmc_cfg = mmc_latch;
                    mmc_cmd_value = 0x3F;
                    break;
                case 0xFD: mmc_cmd_value = 0x00; break;
                case 0xFE:
                    mmc_cmd_value = mmc_heartbeat;
                    mmc_heartbeat ^= 0xFF;
                    break;

                default:
                    // COMPLETE | ERROR_NO_DATA: never leave bit 7 set.
                    mmc_cmd_value = 0x48;
                    break;
            }
            break;

        case 0x01: // latch register
            mmc_latch = value;
            break;

        case 0x03: // write-data register
            if (mmc_accept_write_data && mmc_write_len < sizeof(mmc_data_buf))
                mmc_data_buf[mmc_write_len++] = value;
            break;

        default:
            break;
    }
}
