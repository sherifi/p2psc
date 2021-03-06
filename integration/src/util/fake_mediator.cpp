#include <crypto/rsa.h>
#include <p2psc/log.h>
#include <p2psc/message/advertise.h>
#include <p2psc/message/advertise_abort.h>
#include <p2psc/message/advertise_challenge.h>
#include <p2psc/message/advertise_response.h>
#include <p2psc/message/message_decoder.h>
#include <p2psc/message/peer_disconnect.h>
#include <p2psc/message/peer_identification.h>
#include <src/util/fake_mediator.h>

#define QUIT_IF_REQUESTED(message_type, quit_indicator)                        \
  if (message_type == quit_indicator) {                                        \
    LOG(level::Debug) << "Finishing connection handling (after "               \
                      << message::message_type_string(quit_indicator) << ")";  \
    _shutdown_cv.notify_all();                                                 \
    return;                                                                    \
  }

namespace p2psc {
namespace integration {
namespace util {

FakeMediator::FakeMediator(const SocketCreator &socket_creator)
    : _socket(std::make_unique<socket::LocalListeningSocket>(socket_creator)),
      _mediator(_socket->get_socket_address().ip(),
                _socket->get_socket_address().port()),
      _is_running(false), _protocol_version(kVersion) {}

FakeMediator::FakeMediator(const SocketCreator &socket_creator,
                           const p2psc::Mediator &mediator)
    : _socket(std::make_unique<socket::LocalListeningSocket>(socket_creator)),
      _mediator(mediator), _is_running(false), _protocol_version(kVersion) {}

FakeMediator::~FakeMediator() throw() {
  if (_is_running) {
    stop();
  }
}

void FakeMediator::run() {
  BOOST_ASSERT(!_is_running);
  _is_running = true;
  _worker_thread = std::thread(&FakeMediator::_run, this);
}

void FakeMediator::stop() {
  BOOST_ASSERT(_is_running);
  _is_running = false;
  _socket->close();
  _worker_thread.join();
  for (auto &handler_thread : _handler_pool) {
    handler_thread.join();
  }
}

void FakeMediator::quit_after(message::MessageType message_type) {
  _quit_after = message_type;
}

void FakeMediator::_run() {
  while (_is_running) {
    auto socket = _socket->accept();
    if (!socket) {
      continue;
    }
    _handler_pool.emplace_back(
        std::thread(&FakeMediator::_handle_connection, this, socket));
  }
}

void FakeMediator::_handle_connection(std::shared_ptr<Socket> session_socket) {
  /*
   * Advertise
   */
  const auto advertise = _receive_and_log<message::Advertise>(session_socket);
  QUIT_IF_REQUESTED(advertise.format().type, _quit_after);

  if (advertise.format().payload.version < _protocol_version) {
    const auto advertise_abort =
        Message<message::AdvertiseAbort>(message::AdvertiseAbort{
            "Required protocol version: " + std::to_string(_protocol_version)});
    _send_and_log(session_socket, advertise_abort);
    LOG(level::Error) << "Received protocol version "
                      << advertise.format().payload.version
                      << ", require version " << _protocol_version;
    QUIT_IF_REQUESTED(advertise_abort.format().type, _quit_after);
    return;
  }

  /*
   * AdvertiseChallenge
   */
  const auto nonce = 1337;
  const auto peer_pub_key =
      crypto::RSA::from_public_key(advertise.format().payload.our_key);
  const auto advertise_challenge =
      Message<message::AdvertiseChallenge>(message::AdvertiseChallenge{
          peer_pub_key->public_encrypt(std::to_string(nonce))});
  _send_and_log(session_socket, advertise_challenge);
  QUIT_IF_REQUESTED(advertise_challenge.format().type, _quit_after);

  /*
   * AdvertiseResponse
   */
  const auto advertise_response =
      _receive_and_log<message::AdvertiseResponse>(session_socket);
  QUIT_IF_REQUESTED(advertise_response.format().type, _quit_after);

  const auto maybe_peer =
      _key_to_identifier_store.get(advertise.format().payload.their_key);
  if (!maybe_peer) {
    // In this case, this peer is the Client, and we are waiting for the Peer to
    // come online. We store the Clients address and wait for the other peer to
    // come online so we can send a PeerIdentification back to the Client.
    _key_to_identifier_store.put(
        advertise.format().payload.our_key,
        PeerIdentifier(session_socket->get_socket_address(),
                       advertise.format().payload.version));
    // Timeout after 2 seconds
    const auto awaited_peer = _key_to_identifier_store.await(
        advertise.format().payload.their_key, 2000);
    if (!awaited_peer) {
      // if we never receive an awaited peer, we can't continue
      LOG(level::Error) << "Never received Advertise from peer:" << std::endl
                        << advertise.format().payload.their_key;
      return;
    }

    // We need to make sure we don't send a PeerIdentification to the Client
    // until after the Peer has received its PeerDisconnect, otherwise we can't
    // guarantee that the Peer will be listening for incoming requests
    _wait_for_disconnect(awaited_peer->socket_address);

    /*
     * PeerIdentification
     */
    const auto peer_identification =
        Message<message::PeerIdentification>(message::PeerIdentification{
            awaited_peer->version, awaited_peer->socket_address.ip(),
            awaited_peer->socket_address.port()});
    _send_and_log(session_socket, peer_identification);
    QUIT_IF_REQUESTED(peer_identification.format().type, _quit_after);
  } else {
    // In this case, this peer is the Peer, since the Client has already come
    // online. The Mediator is done with this peer now.
    _key_to_identifier_store.put(
        advertise.format().payload.our_key,
        PeerIdentifier(session_socket->get_socket_address(),
                       advertise.format().payload.version));
    LOG(level::Debug) << "Registered Peer with address: "
                      << session_socket->get_socket_address()
                      << ". Client is already registered as: "
                      << maybe_peer->socket_address;

    /*
     * PeerDisconnect
     */
    const auto socket_address = session_socket->get_socket_address();
    const auto peer_disconnect = Message<message::PeerDisconnect>(
        message::PeerDisconnect{session_socket->get_socket_address().port()});
    _send_and_log(session_socket, peer_disconnect);
    _add_to_disconnects(session_socket->get_socket_address());

    QUIT_IF_REQUESTED(peer_disconnect.format().type, _quit_after);
  }

  _shutdown_cv.notify_all();
}

void FakeMediator::await_shutdown() {
  std::unique_lock<std::mutex> lock(_mutex);
  _disconnect_cv.wait(lock);
}

void FakeMediator::_add_to_disconnects(const socket::SocketAddress &address) {
  {
    std::lock_guard<std::mutex> guard(_mutex);
    _completed_disconnects.insert(address);
  }
  _disconnect_cv.notify_all();
}

void FakeMediator::_wait_for_disconnect(const socket::SocketAddress &address) {
  std::unique_lock<std::mutex> lock(_mutex);
  _disconnect_cv.wait(lock, [&]() {
    return _completed_disconnects.find(address) != _completed_disconnects.end();
  });
  _completed_disconnects.erase(address);
}

template <class T>
void FakeMediator::_send_and_log(std::shared_ptr<Socket> socket,
                                 const Message<T> &message) {
  const auto json = encode(message.format());
  socket->send(json);
  const auto message_type = message::message_type_string(message.format().type);
  _sent_messages.push_back(json);
  LOG(level::Debug) << "Sending " << message_type << " to "
                    << socket->get_socket_address().ip() << ":"
                    << socket->get_socket_address().port() << ": " << json;
}

template <class T>
Message<T> FakeMediator::_receive_and_log(std::shared_ptr<Socket> socket) {
  const auto raw_message = socket->receive();
  auto message = message::decode<T>(raw_message);
  const auto message_type = message::message_type_string(message.type);
  _received_messages.push_back(raw_message);
  LOG(level::Debug) << "Received " << message_type << " from "
                    << socket->get_socket_address().ip() << ":"
                    << socket->get_socket_address().port() << ": "
                    << raw_message;
  return message.payload;
}

p2psc::Mediator FakeMediator::get_mediator_description() const {
  return _mediator;
}

std::vector<std::string> FakeMediator::get_received_messages() const {
  return _received_messages;
}

std::vector<std::string> FakeMediator::get_sent_messages() const {
  return _sent_messages;
}
}
}
}