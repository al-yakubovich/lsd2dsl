#pragma once

#include <vector>
#include <iostream>

namespace dictlsd {

class IRandomAccessStream {
public:
    virtual void readSome(void* dest, unsigned byteCount) = 0;
    virtual void seek(unsigned pos) = 0;
    virtual unsigned tell() = 0;
    virtual ~IRandomAccessStream();
};

class IBitStream : public IRandomAccessStream {
public:
    virtual unsigned read(unsigned len) = 0;
    virtual void toNearestByte() = 0;
    virtual ~IBitStream();
};

class BitStreamAdapter : public IBitStream {
protected:
    IRandomAccessStream* _ras;
    unsigned _bitPos;
    virtual unsigned readBit();
public:
    BitStreamAdapter(IRandomAccessStream* ras);
    virtual unsigned read(unsigned len) override;
    virtual void readSome(void* dest, unsigned byteCount) override;
    virtual void seek(unsigned pos) override;
    virtual void toNearestByte() override;
    virtual unsigned tell() override;
};

class XoringStreamAdapter : public BitStreamAdapter {
    unsigned char _key;
public:
    XoringStreamAdapter(IRandomAccessStream* bstr);
    virtual void readSome(void* dest, unsigned byteCount) override;
    virtual void seek(unsigned pos) override;
protected:
    virtual unsigned readBit() override;
};

class InMemoryStream : public IRandomAccessStream {
    const uint8_t* _buf;
    unsigned _size;
    unsigned _pos;
public:
    InMemoryStream(const void* buf, unsigned size);
    virtual void readSome(void* dest, unsigned byteCount) override;
    virtual void seek(unsigned pos) override;
    virtual unsigned tell() override;
};

}