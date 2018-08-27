/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "Firestore/core/src/firebase/firestore/remote/buffered_writer.h"
#include "Firestore/core/src/firebase/firestore/remote/stream_operation.h"
#include "Firestore/core/src/firebase/firestore/util/async_queue.h"
#include "Firestore/core/src/firebase/firestore/util/status.h"
#include "grpcpp/client_context.h"
#include "grpcpp/generic/generic_stub.h"
#include "grpcpp/support/byte_buffer.h"

namespace firebase {
namespace firestore {
namespace remote {

/** Observer that gets notified of events on a GRPC stream. */
class GrpcStreamObserver {
 public:
  virtual ~GrpcStreamObserver() {
  }

  // Stream has been successfully established.
  virtual void OnStreamStart() = 0;
  // A message has been received from the server.
  virtual void OnStreamRead(const grpc::ByteBuffer& message) = 0;
  // Connection has been broken, perhaps by the server.
  virtual void OnStreamError(const util::Status& status) = 0;

  // Incrementally increasing number used to check whether this observer is
  // still interested in the completion of previously executed operations.
  // GRPC streams are expected to be tagged by a generation number corresponding
  // to the observer; once the observer is no longer interested in that stream,
  // it should increase its generation number.
  virtual int generation() const = 0;
};

/**
 * A gRPC bidirectional stream that notifies the given `observer` about stream
 * events.
 *
 * The stream has to be explicitly opened (via `Start`) before it can be used.
 * The stream is always listening for new messages from the server. The stream
 * can be used to send messages to the server (via `Write`); messages are queued
 * and sent out one by one. Both sent and received messages are raw bytes;
 * serialization and deserialization are left to the caller.
 *
 * The observer will be notified about the following events:
 * - stream has been started;
 * - stream has received a new message from the server;
 * - stream write has finished successfully (which only means it's going on the
 *   wire, not that it has been actually sent);
 * - stream has been interrupted with an error. All errors are unrecoverable.
 * Note that the stream will _not_ notify the observer about finish if the
 * finish was initiated by the client, or about the final write (the write
 * produced by `WriteAndFinish`).
 *
 * The stream stores the generation number of the observer at the time of its
 * creation; once observer increases its generation number, the stream will stop
 * notifying it about events. Moreover, the stream will stop listening to new
 * messages from the server once it notices that the observer increased its
 * generation number. Pending writes will still be sent as normal.
 *
 * The stream is disposable; once it finishes, it cannot be restarted.
 *
 * This class is essentially a wrapper over
 * `grpc::GenericClientAsyncReaderWriter`.
 */
class GrpcStream {
 public:
  using MetadataT = std::unordered_map<std::string, std::string>;

  GrpcStream(std::unique_ptr<grpc::ClientContext> context,
             std::unique_ptr<grpc::GenericClientAsyncReaderWriter> call,
             GrpcStreamObserver* observer,
             util::AsyncQueue* firestore_queue);
  ~GrpcStream();

  void Start();

  void Write(grpc::ByteBuffer&& message);

  // Does not produce a notification. Once this method is called, the stream can
  // no longer be used.
  //
  // Can be called on a stream before it opens. It is invalid to finish a stream
  // more than once.
  void Finish();

  /**
   * Writes the given message and finishes the stream as soon as the write
   * succeeds. Any non-started writes will be discarded. Neither write nor
   * finish will notify the observer.
   *
   * If the stream hasn't opened yet, `WriteAndFinish` is equivalent to
   * `Finish` -- the write will be ignored.
   */
  bool WriteAndFinish(grpc::ByteBuffer&& message);

  bool IsFinished() const { return state_ == State::Finished; }

  MetadataT GetResponseHeaders() const;

  void OnStart();
  void OnRead(const grpc::ByteBuffer& message);
  void OnWrite();
  void OnOperationFailed();
  void OnFinishedByServer(const grpc::Status& status);
  void OnFinishedByClient();
  void RemoveOperation(const StreamOperation* to_remove);

 private:
  void Read();
  StreamWrite* BufferedWrite(grpc::ByteBuffer&& message);
  void FastFinishOperationsBlocking();

  // Whether this stream belongs to the same generation as the observer.
  bool SameGeneration() const;

  // Creates and immediately executes an operation.
  template <typename Op, typename... Args>
  void Execute(Args... args) {
    operations_.push_back(StreamOperation::ExecuteOperation<Op>(
        this, call_.get(), firestore_queue_, args...));
  }

  // The gRPC objects that have to be valid until the last gRPC operation
  // associated with this call finishes. Note that `grpc::ClientContext` is
  // _not_ reference-counted.
  //
  // Important: `call_` has to be destroyed before `context_`, so declaration
  // order matters here. Despite the unique pointer, `call_` is actually
  // a non-owning handle, and the memory it refers to will be released once
  // `context_` (which is owning) is released.
  std::unique_ptr<grpc::ClientContext> context_;
  std::unique_ptr<grpc::GenericClientAsyncReaderWriter> call_;

  util::AsyncQueue* firestore_queue_ = nullptr;

  GrpcStreamObserver* observer_ = nullptr;
  int generation_ = -1;
  BufferedWriter buffered_writer_;

  std::vector<StreamOperation*> operations_;

  // The order of stream states is linear: a stream can never transition to an
  // "earlier" state, only to a "later" one (e.g., stream can go from `Starting`
  // to `Open`, but not vice versa). Intermediate states can be skipped (e.g.,
  // a stream can go from `Starting` directly to `Finishing`).
  enum class State {
    NotStarted,
    Starting,
    Open,
    Finishing,
    Finished
  };
  State state_ = State::NotStarted;

  // For a sanity check
  bool has_pending_read_ = false;
};

}  // namespace remote
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_GRPC_STREAM_H_
