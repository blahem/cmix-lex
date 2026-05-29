#include "encoder.h"

Encoder::Encoder(std::ofstream* os, Predictor* p) : os_(os), x1_(0),
    x2_(0xffffffff), p_(p) {}

void Encoder::WriteByte(unsigned int byte) {
  out_.push_back(byte);
}

unsigned int Encoder::Discretize(float p) {
  return 1 + 65534 * p;
}

void Encoder::Encode(int bit) {
  const unsigned int p = Discretize(p_->Predict());
  const unsigned int xmid = x1_ + ((x2_ - x1_) >> 16) * p +
      (((x2_ - x1_) & 0xffff) * p >> 16);
  if (bit) {
    x2_ = xmid;
  } else {
    x1_ = xmid + 1;
  }
  p_->Perceive(bit);

  while (((x1_^x2_) & 0xff000000) == 0) {
    WriteByte(x2_ >> 24);
    x1_ <<= 8;
    x2_ = (x2_ << 8) + 255;
  }
}

void Encoder::EncodeRawBit(int bit, unsigned int p) {
  if (p < 1) p = 1;
  if (p > 65534) p = 65534;
  const unsigned int xmid = x1_ + ((x2_ - x1_) >> 16) * p +
      (((x2_ - x1_) & 0xffff) * p >> 16);
  if (bit) {
    x2_ = xmid;
  } else {
    x1_ = xmid + 1;
  }

  while (((x1_^x2_) & 0xff000000) == 0) {
    WriteByte(x2_ >> 24);
    x1_ <<= 8;
    x2_ = (x2_ << 8) + 255;
  }
}

void Encoder::ObserveKnownBit(int bit) {
  p_->Predict();
  p_->Perceive(bit);
}

void Encoder::ObserveKnownByte(unsigned int byte) {
  for (int bit = 7; bit >= 0; --bit) {
    ObserveKnownBit((byte >> bit) & 1);
  }
}

void Encoder::BeginTraceByte(unsigned long long offset, unsigned int actual_byte,
    unsigned int prev4) {
  (void)offset;
  (void)actual_byte;
  (void)prev4;
}

void Encoder::EndTraceByte() {}

void Encoder::Flush() {
  while (((x1_^x2_) & 0xff000000) == 0) {
    WriteByte(x2_ >> 24);
    x1_ <<= 8;
    x2_ = (x2_ << 8) + 255;
  }
  WriteByte(x2_ >> 24);

  auto* data = reinterpret_cast<const char*>(out_.data());
  os_->write(data, out_.size());
}
