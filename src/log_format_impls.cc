/**
 * Copyright (c) 2007-2017, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file log_format_impls.cc
 */

#include "config.h"

#include <stdio.h>

#include <utility>

#include "pcrepp/pcrepp.hh"
#include "sql_util.hh"
#include "log_format.hh"
#include "log_vtab_impl.hh"
#include "lnav_util.hh"

using namespace std;

static pcrepp RDNS_PATTERN("^(?:com|net|org|edu|[a-z][a-z])"
                           "(\\.\\w+)+(.+)");

/**
 * Attempt to scrub a reverse-DNS string.
 *
 * @param  str The string to scrub.  If the string looks like a reverse-DNS
 *   string, the leading components of the name will be reduced to a single
 *   letter.  For example, "com.example.foo" will be reduced to "c.e.foo".
 * @return     The scrubbed version of the input string or the original string
 *   if it is not a reverse-DNS string.
 */
static string scrub_rdns(const string &str)
{
    pcre_context_static<30> context;
    pcre_input input(str);
    string     retval;

    if (RDNS_PATTERN.match(context, input)) {
        pcre_context::capture_t *cap;

        cap = context.begin();
        for (int index = 0; index < cap->c_begin; index++) {
            if (index == 0 || str[index - 1] == '.') {
                if (index > 0) {
                    retval.append(1, '.');
                }
                retval.append(1, str[index]);
            }
        }
        retval += input.get_substr(cap);
        retval += input.get_substr(cap + 1);
    }
    else {
        retval = str;
    }
    return retval;
}

class generic_log_format : public log_format {
    static pcrepp &scrub_pattern()
    {
        static pcrepp SCRUB_PATTERN(
            "\\d+-(\\d+-\\d+ \\d+:\\d+:\\d+(?:,\\d+)?:)\\w+:(.*)");

        return SCRUB_PATTERN;
    }

    static pcre_format *get_pcre_log_formats() {
        static pcre_format log_fmt[] = {
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>@[0-9a-zA-Z]{16,24})(.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\dTZ: +/\\-,\\.-]+)([^:]+)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w:+/\\.-]+) \\[\\w (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w:,/\\.-]+) (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w:,/\\.-]+) - (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w: \\.,/-]+) - (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w: \\.,/-]+)\\[[^\\]]+\\](.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?(?<timestamp>[\\w: \\.,/-]+) (.*)"),

            pcre_format(R"(^(?:\*\*\*\s+)?\[(?<timestamp>[\w: \.,+/-]+)\]\s*(\w+):?)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?\\[(?<timestamp>[\\w: \\.,+/-]+)\\] (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?\\[(?<timestamp>[\\w: \\.,+/-]+)\\] \\[(\\w+)\\]"),
            pcre_format("^(?:\\*\\*\\*\\s+)?\\[(?<timestamp>[\\w: \\.,+/-]+)\\] \\w+ (.*)"),
            pcre_format("^(?:\\*\\*\\*\\s+)?\\[(?<timestamp>[\\w: ,+/-]+)\\] \\(\\d+\\) (.*)"),

            pcre_format()
        };

        return log_fmt;
    };

    std::string get_pattern_regex(uint64_t line_number) const {
        int pat_index = this->pattern_index_for_line(line_number);
        return get_pcre_log_formats()[pat_index].name;
    }

    const intern_string_t get_name() const {
        return intern_string::lookup("generic_log");
    };

    void scrub(string &line)
    {
        pcre_context_static<30> context;
        pcre_input pi(line);
        string     new_line;

        if (scrub_pattern().match(context, pi)) {
            pcre_context::capture_t *cap;

            for (cap = context.begin(); cap != context.end(); cap++) {
                new_line += scrub_rdns(pi.get_substr(cap));
            }

            line = new_line;
        }
    };

    scan_result_t scan(logfile &lf,
                       vector<logline> &dst,
                       const line_info &li,
                       shared_buffer_ref &sbr)
    {
        struct exttm log_time;
        struct timeval log_tv;
        pcre_context::capture_t ts, level;
        const char *last_pos;

        if ((last_pos = this->log_scanf(
                dst.size(),
                sbr.get_data(),
                sbr.length(),
                get_pcre_log_formats(),
                nullptr,
                &log_time,
                &log_tv,

                &ts,
                &level)) != nullptr) {
            const char *level_str = &sbr.get_data()[level.c_begin];
            log_level_t level_val = string2level(level_str, level.length());

            if (!((log_time.et_flags & ETF_DAY_SET) &&
                  (log_time.et_flags & ETF_MONTH_SET) &&
                  (log_time.et_flags & ETF_YEAR_SET))) {
                this->check_for_new_year(dst, log_time, log_tv);
            }

            dst.emplace_back(li.li_file_range.fr_offset, log_tv, level_val);
            return SCAN_MATCH;
        }

        return SCAN_NO_MATCH;
    };

    void annotate(uint64_t line_number, shared_buffer_ref &line, string_attrs_t &sa,
                      std::vector<logline_value> &values, bool annotate_module) const
    {
        int pat_index = this->pattern_index_for_line(line_number);
        pcre_format &fmt = get_pcre_log_formats()[pat_index];
        struct line_range lr;
        int prefix_len = 0;
        pcre_input pi(line.get_data(), 0, line.length());
        pcre_context_static<30> pc;

        if (!fmt.pcre.match(pc, pi)) {
            return;
        }

        lr.lr_start = pc[0]->c_begin;
        lr.lr_end   = pc[0]->c_end;
        sa.emplace_back(lr, &logline::L_TIMESTAMP);

        const char *level = &line.get_data()[pc[1]->c_begin];

        if (string2level(level, pc[1]->length(), true) == LEVEL_UNKNOWN) {
            prefix_len = pc[0]->c_end;
        }
        else {
            prefix_len = pc[1]->c_end;
        }

        lr.lr_start = 0;
        lr.lr_end   = prefix_len;
        sa.emplace_back(lr, &logline::L_PREFIX);

        lr.lr_start = prefix_len;
        lr.lr_end   = line.length();
        sa.emplace_back(lr, &SA_BODY);
    };

    shared_ptr<log_format> specialized(int fmt_lock)
    {
        return std::make_shared<generic_log_format>(*this);
    };
};

string from_escaped_string(const char *str, size_t len)
{
    string retval;

    for (size_t lpc = 0; lpc < len; lpc++) {
        switch (str[lpc]) {
            case '\\':
                if ((lpc + 3) < len && str[lpc + 1] == 'x') {
                    int ch;

                    if (sscanf(&str[lpc + 2], "%2x", &ch) == 1) {
                        retval.append(1, (char) ch & 0xff);
                        lpc += 3;
                    }
                }
                break;
            default:
                retval.append(1, str[lpc]);
                break;
        }
    }

    return retval;
}

const char *
lnav_strnstr(const char *s, const char *find, size_t slen)
{
	char c, sc;
	size_t len;

	if ((c = *find++) != '\0') {
		len = strlen(find);
		do {
			do {
				if (slen < 1 || (sc = *s) == '\0')
					return (nullptr);
				--slen;
				++s;
			} while (sc != c);
			if (len > slen)
				return (nullptr);
		} while (strncmp(s, find, len) != 0);
		s--;
	}
	return s;
}

struct separated_string {
    const char *ss_str;
    size_t ss_len;
    const char *ss_separator;
    size_t ss_separator_len;

    explicit separated_string(const char *str = nullptr, size_t len = -1)
        : ss_str(str), ss_len(len), ss_separator(",") {
        this->ss_separator_len = strlen(this->ss_separator);
    };

    separated_string &with_separator(const char *sep) {
        this->ss_separator = sep;
        this->ss_separator_len = strlen(sep);
        return *this;
    };

    struct iterator {
        const separated_string &i_parent;
        const char *i_pos;
        const char *i_next_pos;
        size_t i_index;

        iterator(const separated_string &ss, const char *pos)
            : i_parent(ss), i_pos(pos), i_next_pos(pos), i_index(0) {
            this->update();
        };

        void update() {
            const separated_string &ss = this->i_parent;
            const char *next_field;

            next_field = lnav_strnstr(this->i_pos, ss.ss_separator,
                                      ss.ss_len - (this->i_pos - ss.ss_str));
            if (next_field == nullptr) {
                this->i_next_pos = ss.ss_str + ss.ss_len;
            } else {
                this->i_next_pos = next_field + ss.ss_separator_len;
            }
        };

        iterator &operator++() {
            this->i_pos = this->i_next_pos;
            this->update();
            this->i_index += 1;

            return *this;
        };

        string_fragment operator*() {
            const separated_string &ss = this->i_parent;
            int end;

            if (this->i_next_pos < (ss.ss_str + ss.ss_len)) {
                end = this->i_next_pos - ss.ss_str - ss.ss_separator_len;
            } else {
                end = this->i_next_pos - ss.ss_str;
            }
            return string_fragment(ss.ss_str, this->i_pos - ss.ss_str, end);
        };

        bool operator==(const iterator &other) const {
            return (&this->i_parent == &other.i_parent) &&
                   (this->i_pos == other.i_pos);
        };

        bool operator!=(const iterator &other) const {
            return !(*this == other);
        };

        size_t index() const {
            return this->i_index;
        };
    };

    iterator begin() {
        return {*this, this->ss_str};
    };

    iterator end() {
        return {*this, this->ss_str + this->ss_len};
    };
};

class bro_log_format : public log_format {
public:

    struct field_def {
        const intern_string_t fd_name;
        logline_value::kind_t fd_kind;
        bool fd_identifier;
        std::string fd_collator;
        int fd_numeric_index;

        explicit field_def(const intern_string_t name)
            : fd_name(name),
              fd_kind(logline_value::VALUE_TEXT),
              fd_identifier(false),
              fd_numeric_index(-1) {
        };

        field_def &with_kind(logline_value::kind_t kind,
                             bool identifier = false,
                             const std::string &collator = "") {
            this->fd_kind = kind;
            this->fd_identifier = identifier;
            this->fd_collator = collator;
            return *this;
        };

        field_def &with_numeric_index(int index) {
            this->fd_numeric_index = index;
            return *this;
        }
    };

    bro_log_format() {
        this->lf_is_self_describing = true;
        this->lf_time_ordered = false;
    };

    const intern_string_t get_name() const {
        static const intern_string_t name(intern_string::lookup("bro"));

        return this->blf_format_name.empty() ? name : this->blf_format_name;
    };

    virtual void clear() {
        this->log_format::clear();
        this->blf_format_name.clear();
        this->blf_field_defs.clear();
    };

    scan_result_t scan_int(std::vector<logline> &dst,
                           const line_info &li,
                           shared_buffer_ref &sbr) {
        static const intern_string_t STATUS_CODE = intern_string::lookup("bro_status_code");
        static const intern_string_t TS = intern_string::lookup("bro_ts");
        static const intern_string_t UID = intern_string::lookup("bro_uid");

        separated_string ss(sbr.get_data(), sbr.length());
        struct timeval tv;
        struct exttm tm;
        bool found_ts = false;
        log_level_t level = LEVEL_INFO;
        uint8_t opid = 0;

        ss.with_separator(this->blf_separator.get());

        for (auto iter = ss.begin(); iter != ss.end(); ++iter) {
            if (iter.index() == 0 && *iter == "#close") {
                return SCAN_MATCH;
            }

            if (iter.index() >= this->blf_field_defs.size()) {
                break;
            }

            const field_def &fd = this->blf_field_defs[iter.index()];

            if (TS == fd.fd_name) {
                string_fragment sf = *iter;

                if (this->lf_date_time.scan(sf.data(),
                                            sf.length(),
                                            nullptr,
                                            &tm,
                                            tv)) {
                    this->lf_timestamp_flags = tm.et_flags;
                    found_ts = true;
                }
            } else if (STATUS_CODE == fd.fd_name) {
                string_fragment sf = *iter;

                if (!sf.empty() && sf[0] >= '4') {
                    level = LEVEL_ERROR;
                }
            } else if (UID == fd.fd_name) {
                string_fragment sf = *iter;

                opid = hash_str(sf.data(), sf.length());
            }

            if (fd.fd_numeric_index >= 0) {
                switch (fd.fd_kind) {
                    case logline_value::VALUE_INTEGER:
                    case logline_value::VALUE_FLOAT: {
                        string_fragment sf = *iter;
                        char field_copy[sf.length() + 1];
                        double val;

                        if (sscanf(sf.to_string(field_copy), "%lf", &val) == 1) {
                            this->lf_value_stats[fd.fd_numeric_index].add_value(val);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        if (found_ts) {
            dst.emplace_back(li.li_file_range.fr_offset, tv, level, 0, opid);
            return SCAN_MATCH;
        } else {
            return SCAN_NO_MATCH;
        }
    }

    scan_result_t scan(logfile &lf,
                       std::vector<logline> &dst,
                       const line_info &li,
                       shared_buffer_ref &sbr) {
        static pcrepp SEP_RE(R"(^#separator\s+(.+))");

        if (!this->blf_format_name.empty()) {
            return this->scan_int(dst, li, sbr);
        }

        if (dst.empty() || dst.size() > 20 || sbr.empty() || sbr.get_data()[0] == '#') {
            return SCAN_NO_MATCH;
        }

        pcre_context_static<20> pc;
        auto line_iter = dst.begin();
        auto read_result = lf.read_line(line_iter);

        if (read_result.isErr()) {
            return SCAN_NO_MATCH;
        }

        auto line = read_result.unwrap();
        pcre_input pi(line.get_data(), 0, line.length());

        if (!SEP_RE.match(pc, pi)) {
            return SCAN_NO_MATCH;
        }

        this->clear();

        string sep = from_escaped_string(pi.get_substr_start(pc[0]), pc[0]->length());
        this->blf_separator = intern_string::lookup(sep);

        for (++line_iter; line_iter != dst.end(); ++line_iter) {
            auto next_read_result = lf.read_line(line_iter);

            if (next_read_result.isErr()) {
                return SCAN_NO_MATCH;
            }

            line = next_read_result.unwrap();
            separated_string ss(line.get_data(), line.length());

            ss.with_separator(this->blf_separator.get());
            auto iter = ss.begin();

            string_fragment directive = *iter;

            if (directive.empty() || directive[0] != '#') {
                continue;
            }

            ++iter;
            if (iter == ss.end()) {
                continue;
            }

            if (directive == "#set_separator") {
                this->blf_set_separator = intern_string::lookup(*iter);
            } else if (directive == "#empty_field") {
                this->blf_empty_field = intern_string::lookup(*iter);
            } else if (directive == "#unset_field") {
                this->blf_unset_field = intern_string::lookup(*iter);
            } else if (directive == "#path") {
                string path = to_string(*iter);
                char full_name[128];
                snprintf(full_name, sizeof(full_name), "bro_%s_log", path.c_str());
                this->blf_format_name = intern_string::lookup(full_name);
            } else if (directive == "#fields") {
                do {
                    this->blf_field_defs.emplace_back(intern_string::lookup("bro_" + sql_safe_ident(*iter)));
                    ++iter;
                } while (iter != ss.end());
            } else if (directive == "#types") {
                static const char *KNOWN_IDS[] = {
                    "bro_conn_uids",
                    "bro_fuid",
                    "bro_host",
                    "bro_info_code",
                    "bro_method",
                    "bro_mime_type",
                    "bro_orig_fuids",
                    "bro_parent_fuid",
                    "bro_proto",
                    "bro_referrer",
                    "bro_resp_fuids",
                    "bro_service",
                    "bro_status_code",
                    "bro_uid",
                    "bro_uri",
                    "bro_user_agent",
                    "bro_username",
                };

                int numeric_count = 0;

                do {
                    string_fragment field_type = *iter;
                    field_def &fd = this->blf_field_defs[iter.index() - 1];

                    if (field_type == "time") {
                        fd.with_kind(logline_value::VALUE_TIMESTAMP);
                    } else if (field_type == "string") {
                        bool ident = binary_search(begin(KNOWN_IDS), end(KNOWN_IDS), fd.fd_name);
                        fd.with_kind(logline_value::VALUE_TEXT, ident);
                    } else if (field_type == "count") {
                        bool ident = binary_search(begin(KNOWN_IDS), end(KNOWN_IDS), fd.fd_name);
                        fd.with_kind(logline_value::VALUE_INTEGER, ident)
                          .with_numeric_index(numeric_count);
                        numeric_count += 1;
                    } else if (field_type == "bool") {
                        fd.with_kind(logline_value::VALUE_BOOLEAN);
                    } else if (field_type == "addr") {
                        fd.with_kind(logline_value::VALUE_TEXT, true, "ipaddress");
                    } else if (field_type == "port") {
                        fd.with_kind(logline_value::VALUE_INTEGER, true);
                    } else if (field_type == "interval") {
                        fd.with_kind(logline_value::VALUE_FLOAT)
                          .with_numeric_index(numeric_count);
                        numeric_count += 1;
                    }

                    ++iter;
                } while (iter != ss.end());

                this->lf_value_stats.resize(numeric_count);
            }
        }

        if (!this->blf_format_name.empty() &&
            !this->blf_separator.empty() &&
            !this->blf_field_defs.empty()) {
            dst.clear();
            return this->scan_int(dst, li, sbr);
        }

        this->blf_format_name.clear();
        this->lf_value_stats.clear();

        return SCAN_NO_MATCH;
    };

    void annotate(uint64_t line_number, shared_buffer_ref &sbr, string_attrs_t &sa,
                      std::vector<logline_value> &values, bool annotate_module) const {
        static const intern_string_t TS = intern_string::lookup("bro_ts");
        static const intern_string_t UID = intern_string::lookup("bro_uid");

        separated_string ss(sbr.get_data(), sbr.length());

        ss.with_separator(this->blf_separator.get());

        for (auto iter = ss.begin(); iter != ss.end(); ++iter) {
            if (iter.index() >= this->blf_field_defs.size()) {
                return;
            }

            const field_def &fd = this->blf_field_defs[iter.index()];
            string_fragment sf = *iter;
            logline_value::kind_t kind = fd.fd_kind;

            if (sf == this->blf_empty_field) {
                sf.clear();
            } else if (sf == this->blf_unset_field) {
                sf.invalidate();
                kind = logline_value::VALUE_NULL;
            }

            auto lr = line_range(sf.sf_begin, sf.sf_end);

            if (fd.fd_name == TS) {
                sa.emplace_back(lr, &logline::L_TIMESTAMP);
            } else if (fd.fd_name == UID) {
                sa.emplace_back(lr, &logline::L_OPID);
            }

            if (lr.is_valid()) {
                values.emplace_back(fd.fd_name,
                                    kind,
                                    sbr,
                                    fd.fd_identifier,
                                    nullptr,
                                    iter.index(),
                                    lr.lr_start,
                                    lr.lr_end,
                                    false,
                                    this);
            } else {
                values.emplace_back(fd.fd_name, this);
            }
        }
    };

    const logline_value_stats *stats_for_value(const intern_string_t &name) const {
        const logline_value_stats *retval = nullptr;

        for (size_t lpc = 0; lpc < this->blf_field_defs.size(); lpc++) {
            if (this->blf_field_defs[lpc].fd_name == name) {
                if (this->blf_field_defs[lpc].fd_numeric_index < 0) {
                    break;
                }
                retval = &this->lf_value_stats[this->blf_field_defs[lpc].fd_numeric_index];
                break;
            }
        }

        return retval;
    };

    std::shared_ptr<log_format> specialized(int fmt_lock = -1) {
        return make_shared<bro_log_format>(*this);
    };

    class bro_log_table : public log_format_vtab_impl {
    public:
        bro_log_table(const bro_log_format &format)
            : log_format_vtab_impl(format), blt_format(format) {

        }

        void get_columns(vector<vtab_column> &cols) const {
            for (const auto &fd : this->blt_format.blf_field_defs) {
                std::pair<int, unsigned int> type_pair = log_vtab_impl::logline_value_to_sqlite_type(fd.fd_kind);

                cols.emplace_back(fd.fd_name.to_string(), type_pair.first, fd.fd_collator, false, "", type_pair.second);
            }
        };

        void get_foreign_keys(std::vector<std::string> &keys_inout) const {
            this->log_vtab_impl::get_foreign_keys(keys_inout);

            for (const auto &fd : this->blt_format.blf_field_defs) {
                if (fd.fd_identifier) {
                    keys_inout.push_back(fd.fd_name.to_string());
                }
            }
        }

        const bro_log_format &blt_format;
    };

    static map<intern_string_t, std::shared_ptr<bro_log_table>> &get_tables() {
        static map<intern_string_t, std::shared_ptr<bro_log_table>> retval;

        return retval;
    };

    std::shared_ptr<log_vtab_impl> get_vtab_impl() const {
        if (this->blf_format_name.empty()) {
            return nullptr;
        }

        std::shared_ptr<bro_log_table> retval = nullptr;

        auto &tables = get_tables();
        auto iter = tables.find(this->blf_format_name);
        if (iter == tables.end()) {
            retval = std::make_shared<bro_log_table>(*this);
            tables[this->blf_format_name] = retval;
        }

        return retval;
    };

    void get_subline(const logline &ll,
                     shared_buffer_ref &sbr,
                     bool full_message) {
    }

    intern_string_t blf_format_name;
    intern_string_t blf_separator;
    intern_string_t blf_set_separator;
    intern_string_t blf_empty_field;
    intern_string_t blf_unset_field;
    vector<field_def> blf_field_defs;

};

struct ws_separated_string {
    const char *ss_str;
    size_t ss_len;

    explicit ws_separated_string(const char *str = nullptr, size_t len = -1)
        : ss_str(str), ss_len(len) {
    };

    struct iterator {
        enum class state_t {
            NORMAL,
            QUOTED,
        };

        const ws_separated_string &i_parent;
        const char *i_pos;
        const char *i_next_pos;
        size_t i_index{0};
        state_t i_state{state_t::NORMAL};

        iterator(const ws_separated_string &ss, const char *pos)
            : i_parent(ss), i_pos(pos), i_next_pos(pos) {
            this->update();
        };

        void update() {
            const auto &ss = this->i_parent;
            bool done = false;

            while (!done && this->i_next_pos < (ss.ss_str + ss.ss_len)) {
                switch (this->i_state) {
                    case state_t::NORMAL:
                        if (*this->i_next_pos == '"') {
                            this->i_state = state_t::QUOTED;
                        } else if (isspace(*this->i_next_pos)) {
                            done = true;
                        }
                        break;
                    case state_t::QUOTED:
                        if (*this->i_next_pos == '"') {
                            this->i_state = state_t::NORMAL;
                        }
                        break;
                }
                if (!done) {
                    this->i_next_pos += 1;
                }
            }
        };

        iterator &operator++() {
            const auto &ss = this->i_parent;

            this->i_pos = this->i_next_pos;
            while (this->i_pos < (ss.ss_str + ss.ss_len) &&
                   isspace(*this->i_pos)) {
                this->i_pos += 1;
                this->i_next_pos += 1;
            }
            this->update();
            this->i_index += 1;

            return *this;
        };

        string_fragment operator*() {
            const auto &ss = this->i_parent;
            int end = this->i_next_pos - ss.ss_str;

            return string_fragment(ss.ss_str, this->i_pos - ss.ss_str, end);
        };

        bool operator==(const iterator &other) const {
            return (&this->i_parent == &other.i_parent) &&
                   (this->i_pos == other.i_pos);
        };

        bool operator!=(const iterator &other) const {
            return !(*this == other);
        };

        size_t index() const {
            return this->i_index;
        };
    };

    iterator begin() {
        return {*this, this->ss_str};
    };

    iterator end() {
        return {*this, this->ss_str + this->ss_len};
    };
};

class w3c_log_format : public log_format {
public:

    struct field_def {
        const intern_string_t fd_name;
        const intern_string_t fd_sql_name;
        logline_value::kind_t fd_kind;
        bool fd_identifier;
        std::string fd_collator;
        int fd_numeric_index;

        explicit field_def(const intern_string_t name)
            : fd_name(name),
              fd_sql_name(intern_string::lookup(sql_safe_ident(name.to_string_fragment()))),
              fd_kind(logline_value::VALUE_TEXT),
              fd_identifier(false),
              fd_numeric_index(-1) {
        };

        field_def(const char *name, logline_value::kind_t kind, bool ident = false, std::string coll = "")
            : fd_name(intern_string::lookup(name)),
              fd_sql_name(intern_string::lookup(sql_safe_ident(string_fragment(name)))),
              fd_kind(kind),
              fd_identifier(ident),
              fd_collator(std::move(coll)),
              fd_numeric_index(-1) {

        }

        field_def &with_kind(logline_value::kind_t kind,
                             bool identifier = false,
                             const std::string &collator = "") {
            this->fd_kind = kind;
            this->fd_identifier = identifier;
            this->fd_collator = collator;
            return *this;
        };

        field_def &with_numeric_index(int index) {
            this->fd_numeric_index = index;
            return *this;
        }
    };

    w3c_log_format() {
        this->lf_is_self_describing = true;
        this->lf_time_ordered = false;
    };

    const intern_string_t get_name() const override {
        static const intern_string_t name(intern_string::lookup("w3c"));

        return this->wlf_format_name.empty() ? name : this->wlf_format_name;
    };

    void clear() override {
        this->log_format::clear();
        this->wlf_time_scanner.clear();
        this->wlf_format_name.clear();
        this->wlf_field_defs.clear();
    };

    scan_result_t scan_int(std::vector<logline> &dst,
                           const line_info &li,
                           shared_buffer_ref &sbr) {
        static const intern_string_t DATE = intern_string::lookup("date");
        static const intern_string_t DATE_LOCAL = intern_string::lookup("date-local");
        static const intern_string_t DATE_UTC = intern_string::lookup("date-UTC");
        static const intern_string_t TIME = intern_string::lookup("time");
        static const intern_string_t TIME_LOCAL = intern_string::lookup("time-local");
        static const intern_string_t TIME_UTC = intern_string::lookup("time-UTC");
        static const intern_string_t STATUS_CODE = intern_string::lookup("sc-status");

        ws_separated_string ss(sbr.get_data(), sbr.length());
        struct timeval date_tv{0, 0}, time_tv{0, 0};
        struct exttm date_tm, time_tm;
        bool found_date = false, found_time = false;
        log_level_t level = LEVEL_INFO;

        for (auto iter = ss.begin(); iter != ss.end(); ++iter) {
            if (iter.index() >= this->wlf_field_defs.size()) {
                level = LEVEL_INVALID;
                break;
            }

            const field_def &fd = this->wlf_field_defs[iter.index()];
            string_fragment sf = *iter;

            if (sf.startswith("#")) {
                if (sf == "#Date:") {
                    date_time_scanner dts;
                    struct exttm tm;
                    struct timeval tv;

                    if (dts.scan(sbr.get_data_at(sf.length() + 1),
                                 sbr.length() - sf.length() - 1,
                                 nullptr,
                                 &tm,
                                 tv)) {
                        this->lf_date_time.set_base_time(tv.tv_sec);
                        this->wlf_time_scanner.set_base_time(tv.tv_sec);
                    }
                }
                dst.emplace_back(li.li_file_range.fr_offset, 0, 0, LEVEL_IGNORE, 0);
                return SCAN_MATCH;
            }

            sf.trim("\" \t");
            if (DATE == fd.fd_name ||
                DATE_LOCAL == fd.fd_name ||
                DATE_UTC == fd.fd_name) {
                if (this->lf_date_time.scan(sf.data(),
                                            sf.length(),
                                            nullptr,
                                            &date_tm,
                                            date_tv)) {
                    this->lf_timestamp_flags |= date_tm.et_flags;
                    found_date = true;
                }
            } else if (TIME == fd.fd_name ||
                       TIME_LOCAL == fd.fd_name ||
                       TIME_UTC == fd.fd_name) {
                if (this->wlf_time_scanner.scan(sf.data(),
                                                sf.length(),
                                                nullptr,
                                                &time_tm,
                                                time_tv)) {
                    this->lf_timestamp_flags |= time_tm.et_flags;
                    found_time = true;
                }
            } else if (STATUS_CODE == fd.fd_name) {
                if (!sf.empty() && sf[0] >= '4') {
                    level = LEVEL_ERROR;
                }
            }

            if (fd.fd_numeric_index >= 0) {
                switch (fd.fd_kind) {
                    case logline_value::VALUE_INTEGER:
                    case logline_value::VALUE_FLOAT: {
                        char field_copy[sf.length() + 1];
                        double val;

                        if (sscanf(sf.to_string(field_copy), "%lf", &val) == 1) {
                            this->lf_value_stats[fd.fd_numeric_index].add_value(val);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        if (found_time) {
            struct exttm tm = time_tm;
            struct timeval tv;

            if (found_date) {
                tm.et_tm.tm_year = date_tm.et_tm.tm_year;
                tm.et_tm.tm_mday = date_tm.et_tm.tm_mday;
                tm.et_tm.tm_mon = date_tm.et_tm.tm_mon;
                tm.et_tm.tm_wday = date_tm.et_tm.tm_wday;
                tm.et_tm.tm_yday = date_tm.et_tm.tm_yday;
            }

            tv.tv_sec = tm2sec(&tm.et_tm);
            tv.tv_usec = tm.et_nsec / 1000;

            dst.emplace_back(li.li_file_range.fr_offset, tv, level, 0);
            return SCAN_MATCH;
        } else {
            return SCAN_NO_MATCH;
        }
    }

    scan_result_t scan(logfile &lf,
                       std::vector<logline> &dst,
                       const line_info &li,
                       shared_buffer_ref &sbr) override {
        static const field_def KNOWN_FIELDS[] = {
            {
                "cs-method",
                logline_value::kind_t::VALUE_TEXT,
                true,
            },
            {
                "c-ip",
                logline_value::kind_t::VALUE_TEXT,
                true,
                "ipaddress",
            },
            {
                "cs-bytes",
                logline_value::kind_t::VALUE_INTEGER,
                false,
            },
            {
                "cs-host",
                logline_value::kind_t::VALUE_TEXT,
                true,
            },
            {
                "cs-uri-stem",
                logline_value::kind_t::VALUE_TEXT,
                true,
                "naturalnocase",
            },
            {
                "cs-uri-query",
                logline_value::kind_t::VALUE_TEXT,
                false,
            },
            {
                "cs-username",
                logline_value::kind_t::VALUE_TEXT,
                false,
            },
            {
                "cs-version",
                logline_value::kind_t::VALUE_TEXT,
                true,
            },
            {
                "s-ip",
                logline_value::kind_t::VALUE_TEXT,
                true,
                "ipaddress",
            },
            {
                "s-port",
                logline_value::kind_t::VALUE_INTEGER,
                true,
            },
            {
                "s-computername",
                logline_value::kind_t::VALUE_TEXT,
                true,
            },
            {
                "s-sitename",
                logline_value::kind_t::VALUE_TEXT,
                true,
            },
            {
                "sc-bytes",
                logline_value::kind_t::VALUE_INTEGER,
                false,
            },
            {
                "sc-status",
                logline_value::kind_t::VALUE_INTEGER,
                false,
            },
            {
                "time-taken",
                logline_value::kind_t::VALUE_FLOAT,
                false,
            },
        };

        if (!this->wlf_format_name.empty()) {
            return this->scan_int(dst, li, sbr);
        }

        if (dst.empty() || dst.size() > 20 || sbr.empty() || sbr.get_data()[0] == '#') {
            return SCAN_NO_MATCH;
        }

        this->clear();

        for (auto line_iter = dst.begin(); line_iter != dst.end(); ++line_iter) {
            auto next_read_result = lf.read_line(line_iter);

            if (next_read_result.isErr()) {
                return SCAN_NO_MATCH;
            }

            auto line = next_read_result.unwrap();
            ws_separated_string ss(line.get_data(), line.length());
            auto iter = ss.begin();

            string_fragment directive = *iter;

            if (directive.empty() || directive[0] != '#') {
                continue;
            }

            ++iter;
            if (iter == ss.end()) {
                continue;
            }

            if (directive == "#Date:") {
                date_time_scanner dts;
                struct exttm tm;
                struct timeval tv;

                if (dts.scan(line.get_data_at(directive.length() + 1),
                             line.length() - directive.length() - 1,
                             nullptr,
                             &tm,
                             tv)) {
                    this->lf_date_time.set_base_time(tv.tv_sec);
                    this->wlf_time_scanner.set_base_time(tv.tv_sec);
                }
            } else if (directive == "#Fields:") {
                hasher id_hash;
                int numeric_count = 0;

                do {
                    string_fragment sf = *iter;

                    id_hash.update(sf);
                    sf.trim(")");
                    auto field_iter = std::find_if(begin(KNOWN_FIELDS),
                                                   end(KNOWN_FIELDS),
                                                   [&sf](auto elem) {
                                                       return sf == elem.fd_name;
                                                   });
                    if (field_iter != end(KNOWN_FIELDS)) {
                        this->wlf_field_defs.emplace_back(*field_iter);
                    } else {
                        this->wlf_field_defs.emplace_back(
                            intern_string::lookup(sf));
                    }
                    auto& fd = this->wlf_field_defs.back();
                    switch (fd.fd_kind) {
                        case logline_value::kind_t::VALUE_FLOAT:
                        case logline_value::kind_t::VALUE_INTEGER:
                            fd.with_numeric_index(numeric_count);
                            numeric_count += 1;
                            break;
                        default:
                            break;
                    }

                    ++iter;
                } while (iter != ss.end());

                this->wlf_format_name = intern_string::lookup(fmt::format(
                    "w3c_{}_log", id_hash.to_string().substr(0, 6)));
                this->lf_value_stats.resize(numeric_count);
            }
        }

        if (!this->wlf_format_name.empty() &&
            !this->wlf_field_defs.empty()) {
            dst.clear();
            return this->scan_int(dst, li, sbr);
        }

        this->wlf_format_name.clear();
        this->lf_value_stats.clear();

        return SCAN_NO_MATCH;
    };

    void annotate(uint64_t line_number, shared_buffer_ref &sbr, string_attrs_t &sa,
                  std::vector<logline_value> &values, bool annotate_module) const override {
        ws_separated_string ss(sbr.get_data(), sbr.length());

        for (auto iter = ss.begin(); iter != ss.end(); ++iter) {
            string_fragment sf = *iter;

            if (iter.index() >= this->wlf_field_defs.size()) {
                sa.emplace_back(line_range{sf.sf_begin, -1},
                                &SA_INVALID,
                                (void *) "extra fields detected");
                return;
            }

            const field_def &fd = this->wlf_field_defs[iter.index()];
            logline_value::kind_t kind = fd.fd_kind;

            if (sf == "-") {
                sf.invalidate();
                kind = logline_value::VALUE_NULL;
            }

            auto lr = line_range(sf.sf_begin, sf.sf_end);

            if (lr.is_valid()) {
                values.emplace_back(fd.fd_sql_name,
                                    sf.startswith("\"") ?
                                    logline_value::kind_t::VALUE_W3C_QUOTED :
                                    kind,
                                    sbr,
                                    fd.fd_identifier,
                                    nullptr,
                                    iter.index(),
                                    lr.lr_start,
                                    lr.lr_end,
                                    false,
                                    this);
            } else {
                values.emplace_back(fd.fd_sql_name, this);
            }
        }
    };

    const logline_value_stats *stats_for_value(const intern_string_t &name) const override {
        const logline_value_stats *retval = nullptr;

        for (const auto & wlf_field_def : this->wlf_field_defs) {
            if (wlf_field_def.fd_sql_name == name) {
                if (wlf_field_def.fd_numeric_index < 0) {
                    break;
                }
                retval = &this->lf_value_stats[wlf_field_def.fd_numeric_index];
                break;
            }
        }

        return retval;
    };

    std::shared_ptr<log_format> specialized(int fmt_lock = -1) override {
        return make_shared<w3c_log_format>(*this);
    };

    class w3c_log_table : public log_format_vtab_impl {
    public:
        explicit w3c_log_table(const w3c_log_format &format)
            : log_format_vtab_impl(format), wlt_format(format) {

        }

        void get_columns(vector<vtab_column> &cols) const override {
            for (const auto &fd : this->wlt_format.wlf_field_defs) {
                std::pair<int, unsigned int> type_pair = log_vtab_impl::logline_value_to_sqlite_type(fd.fd_kind);

                cols.emplace_back(fd.fd_sql_name.to_string(), type_pair.first, fd.fd_collator, false, "", type_pair.second);
            }
        };

        void get_foreign_keys(std::vector<std::string> &keys_inout) const override {
            this->log_vtab_impl::get_foreign_keys(keys_inout);

            for (const auto &fd : this->wlt_format.wlf_field_defs) {
                if (fd.fd_identifier) {
                    keys_inout.push_back(fd.fd_sql_name.to_string());
                }
            }
        }

        const w3c_log_format &wlt_format;
    };

    static map<intern_string_t, std::shared_ptr<w3c_log_table>> &get_tables() {
        static map<intern_string_t, std::shared_ptr<w3c_log_table>> retval;

        return retval;
    };

    std::shared_ptr<log_vtab_impl> get_vtab_impl() const override {
        if (this->wlf_format_name.empty()) {
            return nullptr;
        }

        std::shared_ptr<w3c_log_table> retval = nullptr;

        auto &tables = get_tables();
        auto iter = tables.find(this->wlf_format_name);
        if (iter == tables.end()) {
            retval = std::make_shared<w3c_log_table>(*this);
            tables[this->wlf_format_name] = retval;
        }

        return retval;
    };

    void get_subline(const logline &ll,
                     shared_buffer_ref &sbr,
                     bool full_message) override {
    }

    date_time_scanner wlf_time_scanner;
    intern_string_t wlf_format_name;
    vector<field_def> wlf_field_defs;
};

log_format::register_root_format<bro_log_format> bro_log_instance;
log_format::register_root_format<w3c_log_format> w3c_log_instance;
log_format::register_root_format<generic_log_format> generic_log_instance;
