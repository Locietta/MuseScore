/*
MIT License

Copyright (c) 2020 Igor Korsukov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace kors::logger {
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream& operator<<(bool);
    Stream& operator<<(char);
    Stream& operator<<(short);
    Stream& operator<<(unsigned short);
    Stream& operator<<(char16_t);
    Stream& operator<<(char32_t);
    Stream& operator<<(int);
    Stream& operator<<(unsigned int);
    Stream& operator<<(long);
    Stream& operator<<(unsigned long);
    Stream& operator<<(long long);
    Stream& operator<<(unsigned long long);
    Stream& operator<<(float);
    Stream& operator<<(double);
    Stream& operator<<(const void*);
    Stream& operator<<(const char*);
    Stream& operator<<(const std::string&);
    Stream& operator<<(const std::string_view&);
    template <class T>
    Stream& operator<<(const std::vector<T>& values)
    {
        *this << '[';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                *this << ',';
            *this << values[i];
        }
        return *this << ']';
    }
    std::string str() const;

private:
    struct Impl;
    Impl* m_impl;
};
}
