#pragma once
// PC stand-in for Arduino's String (simulator only - see sim/README.md).
// Same behavior as the ESP32 core's WString for every method the clock's
// code uses; backed by std::string.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <string>
#include <algorithm>

class String {
public:
    String() {}
    String(const char* s) { if (s) s_ = s; }
    String(const std::string& s) : s_(s) {}
    String(const String& o) = default;
    String(String&& o) = default;
    explicit String(char c) : s_(1, c) {}
    explicit String(unsigned char v, unsigned char base = 10) { fromULong(v, base); }
    explicit String(int v, unsigned char base = 10) { fromLong(v, base); }
    explicit String(unsigned int v, unsigned char base = 10) { fromULong(v, base); }
    explicit String(long v, unsigned char base = 10) { fromLong(v, base); }
    explicit String(unsigned long v, unsigned char base = 10) { fromULong(v, base); }
    explicit String(long long v, unsigned char base = 10) { fromLong(v, base); }
    explicit String(unsigned long long v, unsigned char base = 10) { fromULong(v, base); }
    explicit String(float v, unsigned int decimals = 2) { fromDouble(v, decimals); }
    explicit String(double v, unsigned int decimals = 2) { fromDouble(v, decimals); }

    String& operator=(const String& o) = default;
    String& operator=(String&& o) = default;
    String& operator=(const char* s) { if (s) s_ = s; else s_.clear(); return *this; }

    const char* c_str() const { return s_.c_str(); }
    const char* begin() const { return s_.c_str(); }
    const char* end() const { return s_.c_str() + s_.size(); }
    char* begin() { return &s_[0]; }
    char* end() { return &s_[0] + s_.size(); }
    unsigned int length() const { return (unsigned int)s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    bool reserve(unsigned int n) { s_.reserve(n); return true; }
    char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    void setCharAt(unsigned int i, char c) { if (i < s_.size()) s_[i] = c; }
    char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    char& operator[](unsigned int i) { static char dummy; if (i >= s_.size()) { dummy = 0; return dummy; } return s_[i]; }

    bool concat(const String& o) { s_ += o.s_; return true; }
    bool concat(const char* s) { if (!s) return false; s_ += s; return true; }
    bool concat(const char* s, unsigned int n) { if (!s) return false; s_.append(s, n); return true; }
    bool concat(char c) { s_ += c; return true; }
    bool concat(unsigned char c) { return concat(String(c)); }
    bool concat(int v) { return concat(String(v)); }
    bool concat(unsigned int v) { return concat(String(v)); }
    bool concat(long v) { return concat(String(v)); }
    bool concat(unsigned long v) { return concat(String(v)); }
    bool concat(long long v) { return concat(String(v)); }
    bool concat(unsigned long long v) { return concat(String(v)); }
    bool concat(float v) { return concat(String(v)); }
    bool concat(double v) { return concat(String(v)); }

    template <typename T> String& operator+=(const T& v) { concat(v); return *this; }

    int compareTo(const String& o) const { return s_.compare(o.s_); }
    bool equals(const String& o) const { return s_ == o.s_; }
    bool equals(const char* s) const { return s && s_ == s; }
    bool equalsIgnoreCase(const String& o) const {
        if (s_.size() != o.s_.size()) return false;
        for (size_t i = 0; i < s_.size(); i++) if (tolower((unsigned char)s_[i]) != tolower((unsigned char)o.s_[i])) return false;
        return true;
    }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0 && s_.size() >= p.s_.size(); }
    bool startsWith(const String& p, unsigned int off) const { return off <= s_.size() && s_.compare(off, p.s_.size(), p.s_) == 0 && s_.size() - off >= p.s_.size(); }
    bool endsWith(const String& p) const { return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0; }

    int indexOf(char c) const { return find(s_.find(c)); }
    int indexOf(char c, unsigned int from) const { return find(s_.find(c, from)); }
    int indexOf(const String& s) const { return find(s_.find(s.s_)); }
    int indexOf(const String& s, unsigned int from) const { return find(s_.find(s.s_, from)); }
    int lastIndexOf(char c) const { return find(s_.rfind(c)); }
    int lastIndexOf(char c, unsigned int from) const { return find(s_.rfind(c, from)); }
    int lastIndexOf(const String& s) const { return find(s_.rfind(s.s_)); }
    int lastIndexOf(const String& s, unsigned int from) const { return find(s_.rfind(s.s_, from)); }

    String substring(unsigned int from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(unsigned int from, unsigned int to) const {
        if (from > to) std::swap(from, to);
        if (from >= s_.size()) return String();
        if (to > s_.size()) to = (unsigned int)s_.size();
        return String(s_.substr(from, to - from));
    }

    void replace(char a, char b) { std::replace(s_.begin(), s_.end(), a, b); }
    void replace(const String& a, const String& b) {
        if (a.s_.empty()) return;
        std::string out; size_t pos = 0, hit;
        while ((hit = s_.find(a.s_, pos)) != std::string::npos) { out.append(s_, pos, hit - pos); out += b.s_; pos = hit + a.s_.size(); }
        out.append(s_, pos, std::string::npos);
        s_ = out;
    }
    void remove(unsigned int index) { if (index < s_.size()) s_.erase(index); }
    void remove(unsigned int index, unsigned int count) { if (index < s_.size()) s_.erase(index, count); }
    void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
    void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
    void trim() {
        size_t b = 0, e = s_.size();
        while (b < e && isspace((unsigned char)s_[b])) b++;
        while (e > b && isspace((unsigned char)s_[e - 1])) e--;
        s_ = s_.substr(b, e - b);
    }
    long toInt() const { return atol(s_.c_str()); }
    float toFloat() const { return (float)atof(s_.c_str()); }
    double toDouble() const { return atof(s_.c_str()); }
    void getBytes(unsigned char* buf, unsigned int n, unsigned int index = 0) const {
        if (!n) return; size_t k = 0;
        for (; k + 1 < n && index + k < s_.size(); k++) buf[k] = (unsigned char)s_[index + k];
        buf[k] = 0;
    }
    void toCharArray(char* buf, unsigned int n, unsigned int index = 0) const { getBytes((unsigned char*)buf, n, index); }

    const std::string& std() const { return s_; }

    friend bool operator==(const String& a, const String& b) { return a.s_ == b.s_; }
    friend bool operator==(const String& a, const char* b) { return b && a.s_ == b; }
    friend bool operator==(const char* a, const String& b) { return a && b.s_ == a; }
    friend bool operator!=(const String& a, const String& b) { return !(a == b); }
    friend bool operator!=(const String& a, const char* b) { return !(a == b); }
    friend bool operator!=(const char* a, const String& b) { return !(a == b); }
    friend bool operator<(const String& a, const String& b) { return a.s_ < b.s_; }
    friend bool operator>(const String& a, const String& b) { return a.s_ > b.s_; }

private:
    std::string s_;
    static int find(size_t p) { return p == std::string::npos ? -1 : (int)p; }
    void fromLong(long long v, unsigned char base) {
        if (base == 10) { char b[32]; snprintf(b, sizeof(b), "%lld", v); s_ = b; return; }
        if (v < 0) { fromULong((unsigned long long)(-v), base); s_ = "-" + s_; } else fromULong((unsigned long long)v, base);
    }
    void fromULong(unsigned long long v, unsigned char base) {
        if (base < 2 || base > 36) base = 10;
        char b[72]; int i = 70; b[71] = 0;
        do { int d = (int)(v % base); b[i--] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
        s_ = &b[i + 1];
    }
    void fromDouble(double v, unsigned int decimals) { char b[64]; snprintf(b, sizeof(b), "%.*f", (int)decimals, v); s_ = b; }
};

// Concatenation, as in Arduino's StringSumHelper (result is a plain String).
inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, char b) { String r(a); r += b; return r; }
inline String operator+(char a, const String& b) { String r; r += a; r += b; return r; }
inline String operator+(const String& a, int b) { String r(a); r += b; return r; }
inline String operator+(const String& a, unsigned int b) { String r(a); r += b; return r; }
inline String operator+(const String& a, long b) { String r(a); r += b; return r; }
inline String operator+(const String& a, unsigned long b) { String r(a); r += b; return r; }
inline String operator+(const String& a, float b) { String r(a); r += b; return r; }
inline String operator+(const String& a, double b) { String r(a); r += b; return r; }
