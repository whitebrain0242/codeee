#include "minimuduo/net/Buffer.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace minimuduo::net {
Buffer::Buffer(std::size_t initialSize)
    : buffer_(kCheapPrepend + initialSize), readerIndex_(kCheapPrepend),
      writerIndex_(kCheapPrepend) {}

std::size_t Buffer::readableBytes() const noexcept {
  return writerIndex_ - readerIndex_;
}

std::size_t Buffer::writableBytes() const noexcept {
  return buffer_.size() - writerIndex_;
}

std::size_t Buffer::prependableBytes() const noexcept { return readerIndex_; }

const char *Buffer::peek() const noexcept { return begin() + readerIndex_; }

char *Buffer::beginWrite() noexcept { return begin() + writerIndex_; }

const char *Buffer::beginWrite() const noexcept {
  return begin() + writerIndex_;
}

void Buffer::retrieve(std::size_t length) {
  assert(length <= readableBytes());
  if (length < readableBytes()) {
    readerIndex_ += length;
  } else {
    retrieveAll();
  }
}

void Buffer::retrieveUntil(const char *end) {
  assert(peek() <= end);
  assert(end <= beginWrite());
  retrieve(static_cast<std::size_t>(end - peek()));
}

void Buffer::retrieveAll() noexcept {
  readerIndex_ = kCheapPrepend;
  writerIndex_ = kCheapPrepend;
}

std::string Buffer::retrieveAsString(std::size_t length) {
  length = std::min(length, readableBytes());
  std::string result(peek(), length);
  retrieve(length);
  return result;
}

std::string Buffer::retrieveAllAsString() {
  return retrieveAsString(readableBytes());
}

const char *Buffer::findEOL() const noexcept {
  const void *result = std::memchr(peek(), '\n', readableBytes());
  return static_cast<const char *>(result);
}

void Buffer::ensureWritableBytes(std::size_t length) {
  if (writableBytes() < length) {
    makeSpace(length);
  }
  assert(writableBytes() >= length);
}

void Buffer::hasWritten(std::size_t length) {
  assert(length <= writableBytes());
  writerIndex_ += length;
}

void Buffer::append(const char *data, std::size_t length) {
  ensureWritableBytes(length);
  std::copy(data, data + length, beginWrite());
  hasWritten(length);
}

void Buffer::append(const std::string &data) {
  append(data.data(), data.size());
}

ssize_t Buffer::readFd(int fd, int *savedErrno) {
  char extraBuffer[65536];

  iovec vectors[2];
  const std::size_t writable = writableBytes();

  vectors[0].iov_base = beginWrite();
  vectors[0].iov_len = writable;
  vectors[1].iov_base = extraBuffer;
  vectors[1].iov_len = sizeof(extraBuffer);

  const int vectorCount = writable < sizeof(extraBuffer) ? 2 : 1;
  const ssize_t bytesRead = ::readv(fd, vectors, vectorCount);

  if (bytesRead < 0) {
    *savedErrno = errno;
  } else if (static_cast<std::size_t>(bytesRead) <= writable) {
    writerIndex_ += static_cast<std::size_t>(bytesRead);
  } else {
    writerIndex_ = buffer_.size();
    append(extraBuffer, static_cast<std::size_t>(bytesRead) - writable);
  }

  return bytesRead;
}

ssize_t Buffer::writeFd(int fd, int *savedErrno) {
  const ssize_t bytesWritten = ::write(fd, peek(), readableBytes());
  if (bytesWritten < 0) {
    *savedErrno = errno;
  }
  return bytesWritten;
}

char *Buffer::begin() noexcept { return buffer_.data(); }

const char *Buffer::begin() const noexcept { return buffer_.data(); }

void Buffer::makeSpace(std::size_t length) {
  if (writableBytes() + prependableBytes() < length + kCheapPrepend) {
    buffer_.resize(writerIndex_ + length);
    return;
  }

  assert(kCheapPrepend < readerIndex_);
  const std::size_t readable = readableBytes();
  std::copy(begin() + readerIndex_, begin() + writerIndex_,
            begin() + kCheapPrepend);
  readerIndex_ = kCheapPrepend;
  writerIndex_ = readerIndex_ + readable;
}
} // namespace minimuduo::net