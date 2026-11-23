// GattSetupAuthorizer.swift
//
// The setup handshake of SetupAuthorizer, carried over the Bluetooth LE peer link instead of RFCOMM: SETUP_REQ
// (central -> phone) and SETUP_ACK (phone -> central) with the same UTF-8 JSON. Two exchanges authorize this Mac:
// the first (revision 1) loses the phone's merge and is answered with the phone's state; the second echoes that
// state back. The phone then marks the session ready, so the existing link carries handover packets.

import Foundation

final class GattSetupAuthorizer {
    enum Failure: Error, LocalizedError {
        case noSession, sendFailed, badReply, timeout
        var errorDescription: String? {
            switch self {
            case .noSession: return "The phone has no Bluetooth LE link to this Mac."
            case .sendFailed: return "Could not send the setup request over Bluetooth LE."
            case .badReply: return "The phone sent an invalid setup answer."
            case .timeout: return "The phone did not answer in time."
            }
        }
    }

    private let registry: PeerRegistry
    private let installationId: String
    private let timeout: TimeInterval = 10
    private var onReply: ((Result<String, Error>) -> Void)?
    private var generation = 0

    init(registry: PeerRegistry, installationId: String) {
        self.registry = registry
        self.installationId = installationId
    }

    /// Runs both exchanges; `completion` is called once, on the main queue.
    func run(completion: @escaping (Result<Void, Error>) -> Void) {
        guard registry.hasLESession(installationId) else {
            Log.write("Authorize: no Bluetooth LE session with the phone")
            return completion(.failure(Failure.noSession))
        }
        registry.onSetupAck = { [weak self] id, payload in
            guard let self, id == self.installationId else { return }
            self.finishExchange(payload)
        }
        let origin = UUID().uuidString.lowercased()
        let first = "{\"version\":1,\"revision\":1,\"origin\":\"\(origin)\",\"enabled\":true}"
        Log.write("Authorize: sending setup request over Bluetooth LE")
        exchange(first) { [weak self] result in
            switch result {
            case .failure(let error): self?.done(.failure(error), completion)
            case .success(let phoneState):
                Log.write("Authorize: phone state \(phoneState); echoing it back")
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                    guard let self else { return }
                    self.exchange(phoneState) { second in self.done(second.map { _ in () }, completion) }
                }
            }
        }
    }

    private func done(_ result: Result<Void, Error>, _ completion: (Result<Void, Error>) -> Void) {
        registry.onSetupAck = nil
        completion(result)
    }

    private func exchange(_ json: String, completion: @escaping (Result<String, Error>) -> Void) {
        let body = Data(json.utf8)
        guard body.count <= PeerGattRecords.maxSetupPayload,
              registry.sendSetupRecord(body, to: installationId) else {
            Log.write("Authorize: failed (\(Failure.sendFailed.localizedDescription))")
            return completion(.failure(Failure.sendFailed))
        }
        generation += 1
        let mine = generation
        onReply = completion
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
            guard let self, self.generation == mine, self.onReply != nil else { return }
            self.finish(.failure(Failure.timeout))
        }
    }

    private func finishExchange(_ payload: Data) {
        guard onReply != nil else { return }
        guard payload.count <= PeerGattRecords.maxSetupPayload,
              (try? JSONSerialization.jsonObject(with: payload)) is [String: Any],
              let text = String(data: payload, encoding: .utf8) else {
            return finish(.failure(Failure.badReply))
        }
        finish(.success(text))
    }

    private func finish(_ result: Result<String, Error>) {
        guard let handler = onReply else { return }
        onReply = nil
        generation += 1
        switch result {
        case .success(let text): Log.write("Authorize: reply \(text)")
        case .failure(let error): Log.write("Authorize: failed (\(error.localizedDescription))")
        }
        handler(result)
    }
}
