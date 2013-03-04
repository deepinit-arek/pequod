#include "pqjoin.hh"
#include "pqserver.hh"
#include "hashtable.hh"
#include "json.hh"
namespace pq {

Match& Match::operator&=(const Match& m) {
    for (int i = 0; i != slot_capacity; ++i) {
        int l = 0;
        while (l != ms_.slotlen_[i] && l != m.ms_.slotlen_[i]
               && slot_[i][l] == m.slot_[i][l])
            ++l;
        ms_.slotlen_[i] = l;
    }
    return *this;
}

std::ostream& operator<<(std::ostream& stream, const Match& m) {
    stream << "{";
    const char* sep = "";
    for (int i = 0; i != slot_capacity; ++i)
	if (m.slotlen(i)) {
	    stream << sep << i << ": ";
	    stream.write(reinterpret_cast<const char*>(m.slot(i)),
			 m.slotlen(i));
	    sep = ", ";
	}
    return stream << "}";
}

void Pattern::append_literal(uint8_t ch) {
    assert(ch < 128);
    assert(plen_ < pcap);
    pat_[plen_] = ch;
    ++plen_;
    ++klen_;
}

void Pattern::append_slot(int si, int len) {
    assert(plen_ < pcap);
    assert(si >= 0 && si < slot_capacity);
    assert(!has_slot(si));
    assert(len != 0);
    slotlen_[si] = len;
    slotpos_[si] = klen_;
    pat_[plen_] = 128 + si;
    ++plen_;
    klen_ += len;
}

SourceRange* Join::make_source(Server& server, const Match& m,
                               Str ibegin, Str iend) {
    if (jvt() == jvt_copy_last)
        return new CopySourceRange(server, this, m, ibegin, iend);
    else if (jvt() == jvt_count_match)
        return new CountSourceRange(server, this, m, ibegin, iend);
    else if (jvt() == jvt_min_last)
        return new MinSourceRange(server, this, m, ibegin, iend);
    else if (jvt() == jvt_max_last)
        return new MaxSourceRange(server, this, m, ibegin, iend);
    else
        assert(0);
}

SourceAccumulator* Join::make_accumulator(Server& server) {
    if (jvt() == jvt_count_match)
        return new CountSourceAccumulator(&server.make_table(sink().table_name()));
    else if (jvt() == jvt_min_last)
        return new MinSourceAccumulator(&server.make_table(sink().table_name()));
    else if (jvt() == jvt_max_last)
        return new MaxSourceAccumulator(&server.make_table(sink().table_name()));
    else
        return 0;
}

bool Pattern::assign_parse(Str str, HashTable<Str, int> &slotmap) {
    plen_ = klen_ = 0;
    for (auto s = str.ubegin(); s != str.uend(); ) {
	if (plen_ == pcap)
	    return false;
	if (*s != '<') {
	    append_literal(*s);
	    ++s;
	} else {
	    ++s;
	    // parse name up to length description
	    auto name0 = s;
	    while (s != str.uend() && *s != ':' && *s != '>')
		++s;
	    if (s == name0) {
                printf("assign_parse(): Malformed name\n");
		return false;
            }
	    Str name(name0, s);
            if (*s != ':' && slotmap.get(name) == -1) {
                std::cout << "assign_parse(): No length description for " << name << std::endl;
		return false;
            }
	    // parse name description
	    int len = 0;
	    if (*s == ':' && (s + 1 == str.uend() || !isdigit(s[1]))) {
                std::cout << "assign_parse(): Malformed length description\n";
		return false;
            }
	    else if (*s == ':') {
		len = s[1] - '0';
		for (s += 2; s != str.uend() && isdigit(*s); ++s)
		    len = 10 * len + *s - '0';
	    }
	    if (s == str.uend() || *s != '>') {
                std::cout << "assign_parse(): Bad end\n";
		return false;
            }
	    ++s;
	    // look up slot, maybe store it in map
	    int slot = slotmap.get(name);
	    if (slot == -1) {
		if (len == 0 || slotmap.size() == slot_capacity) {
                    std::cout << "assign_parse(): Reached capacity " << slot_capacity << std::endl;
		    return false;
                }
		slot = len + 256 * slotmap.size();
		slotmap.set(name, slot);
	    } else if (len != 0 && len != (slot & 255))
		return false;
	    // add slot
	    append_slot(slot >> 8, slot & 255);
	}
    }
    return true;
}

bool Pattern::assign_parse(Str str) {
    HashTable<Str, int> slotmap(-1);
    return assign_parse(str, slotmap);
}

bool Join::assign_parse(Str str) {
    HashTable<Str, int> slotmap(-1);
    npat_ = 0;
    auto s = str.ubegin();
    while (1) {
	while (s != str.uend() && isspace(*s))
	    ++s;
	auto pbegin = s;
	while (s != str.uend() && !isspace(*s))
	    ++s;
	if (pbegin == s)
            return analyze();
	if (!pat_[npat_].assign_parse(Str(pbegin, s), slotmap))
	    return false;
        // Every pattern must involve a known table
        if (!table_name(pat_[npat_].expand_first(Match())))
            return false;
	++npat_;
    }
}

bool Join::analyze() {
    // fail if too few patterns
    if (npat_ <= 1)
        return false;

    // account for slots across all patterns
    // completion_source_ is the source number after which sink() is complete
    int need_slots = 0;
    for (int s = 0; s < slot_capacity; ++s)
        if (sink().has_slot(s))
            need_slots |= 1 << s;

    completion_source_ = -1;
    while (need_slots) {
        ++completion_source_;
        if (completion_source_ >= nsource())
            return false;
        for (int s = 0; s < slot_capacity; ++s)
            if (source(completion_source_).has_slot(s)
                && source(completion_source_).slot_length(s) == sink().slot_length(s))
                need_slots &= ~(1 << s);
    }

    if (need_slots)
        return false;

    // success
    return true;
}

Json Pattern::unparse_json() const {
    Json j = Json::make_array();
    const uint8_t* p = pat_;
    while (p != pat_ + plen_) {
	if (*p >= 128) {
	    j.push_back(Json::make_array(*p - 127, slotlen_[*p - 128]));
	    ++p;
	} else {
	    const uint8_t* pfirst = p;
	    for (++p; p != pat_ + plen_ && *p < 128; ++p)
		/* do nothing */;
	    j.push_back(String(pfirst, p));
	}
    }
    return j;
}

String Pattern::unparse() const {
    return unparse_json().unparse();
}

std::ostream& operator<<(std::ostream& stream, const Pattern& m) {
    stream << "{";
    const uint8_t* p = m.pat_;
    while (p != m.pat_ + m.plen_) {
        if (*p >= 128) {
            stream << "<pos: " << (int)m.slotpos_[*p - 128]
                   << " len: " << (int)m.slotlen_[*p - 128] << ">";
            ++p;
        } else {
            const uint8_t* pfirst = p;
            for (++p; p != m.pat_ + m.plen_ && *p < 128; ++p)
                /* do nothing */;
            stream << String(pfirst, p);
        }
    }
    return stream << "}";
}

Json Join::unparse_json() const {
    Json j = Json::make_array();
    for (int i = 0; i < npat_; ++i)
	j.push_back(pat_[i].unparse_json());
    return j;
}

String Join::unparse() const {
    return unparse_json().unparse();
}

std::ostream& operator<<(std::ostream& stream, const Join& join) {
    return stream << join.unparse();
}

} // namespace
