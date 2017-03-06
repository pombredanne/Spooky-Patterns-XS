#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "SpookyV2.h"
#include "spooky_patterns.h"
#include <boost/tokenizer.hpp>
#include <cstring>
#include <iostream>
#include <list>
#include <map>

#define DEBUG 0

// typical comment and markup - have to be single tokens!
const char* ignored_tokens[] = { "*", "/*", "*/", "//", "%", "%%", "dnl",
    "//**", "/**", "-", "#", "**", "#~", ";", ";;", "=", ",",
    "\"", "\"\"", "--", "#:", "{", "\\", ">", ":", "==", "!", "::",
    "##", "|", "+", 0 };

bool to_ignore(const char* token)
{
    int index = 0;
    while (ignored_tokens[index]) {
        if (!strcmp(token, ignored_tokens[index]))
            return true;
        index++;
    }
    return false;
}

struct Token {
    int linenumber;
    uint64_t hash;
    std::string text;
};

typedef std::vector<Token> TokenList;

struct Pattern {
    TokenList tokens;
    std::string name;
    int id;
};

typedef std::vector<Pattern> PatternList;

const int MAX_TOKEN_LENGTH = 100;

void tokenize(TokenList& result, const std::string& str, int linenumber = 0)
{
    char copy[MAX_TOKEN_LENGTH];

    typedef boost::tokenizer<boost::char_separator<char> >
        tokenizer;
    // drop whitespace, but keep punctation in the token flow - mostly to be ignored
    boost::char_separator<char> sep(" \r\n\t", ":-,;.+!?\"\'#");
    tokenizer tokens(str, sep);
    for (tokenizer::iterator tok_iter = tokens.begin();
         tok_iter != tokens.end(); ++tok_iter) {
        size_t len = tok_iter->copy(copy, MAX_TOKEN_LENGTH - 1);
        copy[len] = 0;
        if (to_ignore(copy))
            continue;
        for (unsigned int i = 0; i < len; i++)
            copy[i] = tolower(copy[i]);
        Token t;
        t.text = copy;
        t.linenumber = linenumber;
        t.hash = 0;
        if (!linenumber && copy[0] == '$') {
            if (!strcmp(copy, "$owner")) {
                t.hash = 10;
            } else if (!strcmp(copy, "$year")) {
                t.hash = 1;
            } else if (!strcmp(copy, "$var")) {
                t.hash = 2;
            } else if (!strcmp(copy, "$varl")) {
                t.hash = 19;
            } else if (!strcmp(copy, "$ownerl")) {
                t.hash = 19;
            } else if (!strcmp(copy, "$years_owner")) {
                t.hash = 15;
            } else if (!strcmp(copy, "$years")) {
                // 2015,2016,2017 is still only one token for us, but
                // better save than sorry
                t.hash = 8;
            }
        }
        if (!t.hash) {
            // hash64 has no collisions on our patterns and is very fast
            // *and* 0-20 are "free"
            t.hash = SpookyHash::Hash64(copy, len, 1);
            assert(t.hash > 20);
        }
        result.push_back(t);
    }
}

AV* pattern_parse(const char* str)
{
    TokenList t;
    tokenize(t, str);
    AV* ret = newAV();
    av_extend(ret, t.size());
    int index = 0;
    for (TokenList::const_iterator it = t.begin(); it != t.end(); ++it, ++index) {
        av_store(ret, index, newSVuv(it->hash));
    }
    return ret;
}

typedef std::map<uint64_t, PatternList> PatternHash;

struct Matcher {
    PatternHash patterns;
};

Matcher* pattern_init_matcher()
{
    return new Matcher;
}

void destroy_matcher(Matcher* m)
{
    delete m;
}

void pattern_add(Matcher* m, unsigned int id, av* tokens)
{
    Pattern p;
    p.id = id;

    SSize_t len = av_top_index(tokens) + 1;
    if (!len) {
        std::cout << "add failed for id " << id << std::endl;
        return;
    }
    p.tokens.reserve(len);

    Token t;
    for (SSize_t i = 0; i < len; ++i) {
        SV* sv = *av_fetch(tokens, i, 0);
        UV uv = SvUV(sv);
        t.hash = uv;
        p.tokens.push_back(t);
    }

    m->patterns[p.tokens[0].hash].push_back(p);
}

int match_pattern(const TokenList& tokens, unsigned int offset, const Pattern& p)
{
    unsigned int index = 0;
    TokenList::const_iterator pat_iter = p.tokens.begin();

    while (pat_iter != p.tokens.end()) {
        // pattern longer than text -> fail
        if (offset + index >= tokens.size())
            return 0;

#if DEBUG
	printf("MP %d %d %s<->%s %lx<->%lx\n", offset,index,tokens[offset+index].text.c_str(),
	       pat_iter->text.c_str(), tokens[offset+index].hash, pat_iter->hash);
#endif
	
        if (pat_iter->hash < 20) {
            int max_gap = pat_iter->hash;
            pat_iter++;
            do {
                // skip at least one word
                index++;
                if (offset + index >= tokens.size())
                    return 0;
                if (max_gap-- == 0) {
                    return 0;
                }
		
#if DEBUG
		printf("MP2 %d+%d %d %s<->%s %lx<->%lx\n", offset,max_gap,index,tokens[offset+index].text.c_str(),
	       pat_iter->text.c_str(), tokens[offset+index].hash, pat_iter->hash);
#endif
                // we need to stop on further variables
                if (pat_iter->hash <= 20)
                    break;
            } while (tokens[offset + index].hash != pat_iter->hash);
        } else {
            if (tokens[offset + index].hash != pat_iter->hash) {
                return 0;
            }
            index++;
            pat_iter++;
        }
    }
    return index;
}

struct Match {
    int line_start;
    int line_end;
    std::string lic_name;
    int pattern;

    int line_diff() const { return line_end - line_start; }
};

// if either the start or the end of one region is within the other
bool line_overlap(int s1, int e1, int s2, int e2)
{
    if (s1 >= s2 && s1 <= e2)
        return true;
    if (e1 >= s2 && e1 <= e2)
        return true;
    return false;
}

typedef std::list<Match> Matches;

AV* pattern_find_matches(Matcher* m, const char* filename)
{
    AV* ret = newAV();

    FILE* input = fopen(filename, "r");
    if (!input) {
        std::cerr << "Failed to open " << filename << std::endl;
        return ret;
    }
    char line[1000];
    int linenumber = 1;
    TokenList ts;
    while (fgets(line, sizeof(line) - 1, input)) {
        tokenize(ts, line, linenumber++);
    }
    fclose(input);

    Matches ms;
    for (unsigned int i = 0; i < ts.size(); i++) {
        PatternHash::const_iterator it = m->patterns.find(ts[i].hash);
        //std::cout << ts[i].text << " " << (it == m->patterns.end()) << std::endl;
        if (it == m->patterns.end())
            continue;
        PatternList::const_iterator it2 = it->second.begin();
        //printf("T %s:%d:%lx %d %s\n", filename, ts[i].linenumber, ts[i].hash, it->second.size(), ts[i].text.c_str());
        for (; it2 != it->second.end(); ++it2) {
            int matched = match_pattern(ts, i, *it2);
            if (matched) {
                Match m;
                m.line_start = ts[i].linenumber;
                m.line_end = ts[i + matched - 1].linenumber;
                m.pattern = it2->id;
                m.lic_name = it2->name;
                //	printf("L %s:%d-%d %s (%d)\n", filename, m.line_start, m.line_end, it2->name.c_str(), it2->id);
                ms.push_back(m);
            }
        }
    }
    Matches bests;
    while (ms.size()) {
        Matches::const_iterator it = ms.begin();
        Match best = *(it++);
        for (; it != ms.end(); ++it) {
            if (best.line_diff() < it->line_diff()) {
                best = *it;
            }
        }
        //std::cout << filename << " " << best.lic_name << "(" << best.pattern  << ") " << best.line_start << ":" << best.line_end << std::endl;
        bests.push_back(best);
        for (Matches::iterator it2 = ms.begin(); it2 != ms.end();) {
            if (line_overlap(it2->line_start, it2->line_end, best.line_start, best.line_end))
                it2 = ms.erase(it2);
            else
                it2++;
        }
    }

    int index = 0;
    for (Matches::const_iterator it = bests.begin(); it != bests.end(); ++it, ++index) {
      AV *line = newAV();
        av_push(line, newSVuv(it->pattern));
        av_push(line, newSVuv(it->line_start));
        av_push(line, newSVuv(it->line_end));
	av_push(ret, newRV_noinc((SV*)line));
    }
    return ret;
}

AV* pattern_read_lines(const char* filename, int from, int to)
{
    AV* ret = newAV();

    FILE* input = fopen(filename, "r");
    if (!input) {
        std::cerr << "Failed to open " << filename << std::endl;
        return ret;
    }
    char line[1000];
    int linenumber = 1;
    TokenList ts;
    while (fgets(line, sizeof(line) - 1, input)) {
        if (linenumber >= from) {
            // fgets makes sure we have a 0 at the end
            size_t len = strlen(line);
            // remove one char (most likely newline)
            line[--len] = 0;
            av_push(ret, newSVpv(line, len));
        }
        if (++linenumber > to)
            break;
    }
    fclose(input);
    return ret;
}
