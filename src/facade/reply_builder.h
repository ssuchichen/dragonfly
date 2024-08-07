// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <absl/container/flat_hash_map.h>

#include <optional>
#include <string_view>

#include "facade/facade_types.h"
#include "facade/op_status.h"
#include "io/io.h"

namespace facade {

// Reply mode allows filtering replies.
enum class ReplyMode {
  NONE,      // No replies are recorded
  ONLY_ERR,  // Only errors are recorded
  FULL       // All replies are recorded
};

class SinkReplyBuilder {
 public:
  struct MGetStorage {
    MGetStorage* next = nullptr;
    char data[1];
  };

  struct GetResp {
    std::string key;  // TODO: to use backing storage to optimize this as well.
    std::string_view value;

    uint64_t mc_ver = 0;  // 0 means we do not output it (i.e has not been requested).
    uint32_t mc_flag = 0;

    GetResp() = default;
    GetResp(std::string_view val) : value(val) {
    }
  };

  struct MGetResponse {
    MGetStorage* storage_list = nullptr;  // backing storage of resp_arr values.
    std::vector<std::optional<GetResp>> resp_arr;

    MGetResponse() = default;

    MGetResponse(size_t size) : resp_arr(size) {
    }

    ~MGetResponse();

    MGetResponse(MGetResponse&& other) noexcept
        : storage_list(other.storage_list), resp_arr(std::move(other.resp_arr)) {
      other.storage_list = nullptr;
    }

    MGetResponse& operator=(MGetResponse&& other) noexcept {
      resp_arr = std::move(other.resp_arr);
      storage_list = other.storage_list;
      other.storage_list = nullptr;
      return *this;
    }
  };

  SinkReplyBuilder(const SinkReplyBuilder&) = delete;
  void operator=(const SinkReplyBuilder&) = delete;

  explicit SinkReplyBuilder(::io::Sink* sink);

  virtual ~SinkReplyBuilder() {
  }

  static MGetStorage* AllocMGetStorage(size_t size) {
    static_assert(alignof(MGetStorage) == 8);  // if this breaks we should fix the code below.
    char* buf = new char[size + sizeof(MGetStorage)];
    return new (buf) MGetStorage();
  }

  virtual void SendError(std::string_view str, std::string_view type = {}) = 0;  // MC and Redis
  virtual void SendError(OpStatus status);
  void SendError(ErrorReply error);

  virtual void SendStored() = 0;  // Reply for set commands.
  virtual void SendSetSkipped() = 0;

  virtual void SendMGetResponse(MGetResponse resp) = 0;

  virtual void SendLong(long val) = 0;
  virtual void SendSimpleString(std::string_view str) = 0;

  void SendOk() {
    SendSimpleString("OK");
  }

  virtual void SendProtocolError(std::string_view str) = 0;

  // In order to reduce interrupt rate we allow coalescing responses together using
  // Batch mode. It is controlled by Connection state machine because it makes sense only
  // when pipelined requests are arriving.
  void SetBatchMode(bool batch);

  void FlushBatch();

  // Used for QUIT - > should move to conn_context?
  void CloseConnection();

  std::error_code GetError() const {
    return ec_;
  }

  bool IsSendActive() const {
    return send_active_;
  }

  struct ReplyAggregator {
    explicit ReplyAggregator(SinkReplyBuilder* builder) : builder_(builder) {
      // If the builder is already aggregating then don't aggregate again as
      // this will cause redundant sink writes (such as in a MULTI/EXEC).
      if (builder->should_aggregate_) {
        return;
      }
      builder_->StartAggregate();
      is_nested_ = false;
    }

    ~ReplyAggregator() {
      if (!is_nested_) {
        builder_->StopAggregate();
      }
    }

   private:
    SinkReplyBuilder* builder_;
    bool is_nested_ = true;
  };

  void ExpectReply();
  bool HasReplied() const;

  virtual size_t UsedMemory() const;

  static const ReplyStats& GetThreadLocalStats() {
    return tl_facade_stats->reply_stats;
  }

  static void ResetThreadLocalStats();

 protected:
  void SendRaw(std::string_view str);  // Sends raw without any formatting.

  void Send(const iovec* v, uint32_t len);

  void StartAggregate();
  void StopAggregate();

  std::string batch_;
  ::io::Sink* sink_;
  std::error_code ec_;

  bool should_batch_ : 1;

  // Similarly to batch mode but is controlled by at operation level.
  bool should_aggregate_ : 1;
  bool has_replied_ : 1;
  bool send_active_ : 1;
};

// TMP: New version of reply builder that batches not only to a buffer, but also iovecs.
class SinkReplyBuilder2 {
  explicit SinkReplyBuilder2(io::Sink* sink) : sink_(sink) {
  }

  // Use with care: All send calls within a scope must keep their data alive!
  // This allows to fully eliminate copies for batches of data by using vectorized io.
  struct ReplyScope {
    explicit ReplyScope(SinkReplyBuilder2* rb) : prev_scoped(rb->scoped_), rb(rb) {
      rb->scoped_ = true;
    }
    ~ReplyScope() {
      if (!prev_scoped) {
        rb->scoped_ = false;
        rb->FinishScope();
      }
    }

   private:
    bool prev_scoped;
    SinkReplyBuilder2* rb;
  };

 public:
  void Write(std::string_view str);

 protected:
  void Flush();        // Send all accumulated data and reset to clear state
  void FinishScope();  // Called when scope ends

  char* ReservePiece(size_t size);        // Reserve size bytes from buffer
  void CommitPiece(size_t size);          // Mark size bytes from buffer as used
  void WritePiece(std::string_view str);  // Reserve + memcpy + Commit

  void WriteRef(std::string_view str);  // Add iovec bypassing buffer

  bool IsInBuf(const void* ptr) const;  // checks if ptr is part of buffer_
  void NextVec(std::string_view str);

 private:
  io::Sink* sink_;
  std::error_code ec_;

  bool scoped_;

  size_t total_size_ = 0;  // sum of vec_ lengths
  base::IoBuf buffer_;
  std::vector<iovec> vecs_;
};

class MCReplyBuilder : public SinkReplyBuilder {
  bool noreply_;

 public:
  MCReplyBuilder(::io::Sink* stream);

  using SinkReplyBuilder::SendRaw;

  void SendError(std::string_view str, std::string_view type = std::string_view{}) final;

  // void SendGetReply(std::string_view key, uint32_t flags, std::string_view value) final;
  void SendMGetResponse(MGetResponse resp) final;

  void SendStored() final;
  void SendLong(long val) final;
  void SendSetSkipped() final;

  void SendClientError(std::string_view str);
  void SendNotFound();
  void SendSimpleString(std::string_view str) final;
  void SendProtocolError(std::string_view str) final;

  void SetNoreply(bool noreply) {
    noreply_ = noreply;
  }

  bool NoReply() const;
};

class RedisReplyBuilder : public SinkReplyBuilder {
 public:
  enum CollectionType { ARRAY, SET, MAP, PUSH };

  enum VerbatimFormat { TXT, MARKDOWN };

  using StrSpan = facade::ArgRange;

  RedisReplyBuilder(::io::Sink* stream);

  void SetResp3(bool is_resp3);
  bool IsResp3() const {
    return is_resp3_;
  }

  void SendError(std::string_view str, std::string_view type = {}) override;
  using SinkReplyBuilder::SendError;

  void SendMGetResponse(MGetResponse resp) override;

  void SendStored() override;
  void SendSetSkipped() override;
  void SendProtocolError(std::string_view str) override;

  virtual void SendNullArray();   // Send *-1
  virtual void SendEmptyArray();  // Send *0
  virtual void SendSimpleStrArr(StrSpan arr);
  virtual void SendStringArr(StrSpan arr, CollectionType type = ARRAY);

  virtual void SendNull();
  void SendLong(long val) override;
  virtual void SendDouble(double val);
  void SendSimpleString(std::string_view str) override;

  virtual void SendBulkString(std::string_view str);
  virtual void SendVerbatimString(std::string_view str, VerbatimFormat format = TXT);
  virtual void SendScoredArray(const std::vector<std::pair<std::string, double>>& arr,
                               bool with_scores);

  void StartArray(unsigned len);  // StartCollection(len, ARRAY)

  virtual void StartCollection(unsigned len, CollectionType type);

  static char* FormatDouble(double val, char* dest, unsigned dest_len);

 private:
  void SendStringArrInternal(size_t size, absl::FunctionRef<std::string_view(unsigned)> producer,
                             CollectionType type);

  bool is_resp3_ = false;
};

class ReqSerializer {
 public:
  explicit ReqSerializer(::io::Sink* stream) : sink_(stream) {
  }

  void SendCommand(std::string_view str);

  std::error_code ec() const {
    return ec_;
  }

 private:
  ::io::Sink* sink_;
  std::error_code ec_;
};

}  // namespace facade
