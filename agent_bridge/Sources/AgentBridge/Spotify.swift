// Spotify for the display's music page: sign in here (Spotify's approval page in the browser, PKCE: no password or
// client secret involved), and the display gets a refresh token of its own (POST /spotify) and talks to Spotify itself
// from then on. Spotify sends you back to http://127.0.0.1:9900/ (registered for this client ID), caught by a
// listener that's up only while signing in.
import AppKit
import CryptoKit
import Network

enum Spotify {
    static let clientID = "94a34d8f859244cf9fb932d8b8a7ac74"
    static let redirect = "http://127.0.0.1:9900/"
    static let scopes = "user-read-playback-state user-modify-playback-state user-read-currently-playing playlist-read-private " +
        "playlist-read-collaborative user-library-read user-read-playback-position"

    enum Failure: Error, CustomStringConvertible {
        case declined(String), noToken, display
        var description: String {
            switch self {
            case .declined(let why): return "Spotify didn't approve (\(why))."
            case .noToken: return "Spotify didn't hand over access. Try again."
            case .display: return "The display didn't take it. Make sure it's on and paired."
            }
        }
    }

    private static var listener: NWListener?

    // Sign in and hand the result to the display; calls back on the main queue
    static func connect(_ device: Device, _ done: @escaping (Result<Void, Error>) -> Void) {
        let verifier = randomURLSafe(48)
        let challenge = Data(SHA256.hash(data: Data(verifier.utf8))).base64URL
        let state = randomURLSafe(16)
        listener?.cancel()
        do {
            let params = NWParameters.tcp
            params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: 9900)
            params.allowLocalEndpointReuse = true
            let l = try NWListener(using: params)
            l.newConnectionHandler = { c in
                c.start(queue: .main)
                HTTP.readMessage(c) { req in
                    // "GET /?code=...&state=... HTTP/1.1"
                    let path = req?.first.split(separator: " ").dropFirst().first.map(String.init) ?? ""
                    let q = URLComponents(string: "http://x\(path)")?.queryItems ?? []
                    let value = { (k: String) in q.first { $0.name == k }?.value }
                    guard value("state") == state else { reply(c, "Something went wrong: please try again from the Bobble menu."); return }
                    listener?.cancel()
                    listener = nil
                    if let code = value("code") {
                        reply(c, "Spotify's connected to your Bobble. You can close this tab.")
                        exchange(code, verifier) { result in
                            switch result {
                            case .failure(let e): done(.failure(e))
                            case .success(let refresh):
                                device.setSpotify(clientID: clientID, refreshToken: refresh) { ok in
                                    done(ok ? .success(()) : .failure(Failure.display))
                                }
                            }
                        }
                    } else {
                        reply(c, "Not connected.")
                        done(.failure(Failure.declined(value("error") ?? "cancelled")))
                    }
                }
            }
            l.start(queue: .main)
            listener = l
        } catch {
            done(.failure(error))
            return
        }
        var url = URLComponents(string: "https://accounts.spotify.com/authorize")!
        url.queryItems = [
            .init(name: "client_id", value: clientID), .init(name: "response_type", value: "code"),
            .init(name: "redirect_uri", value: redirect), .init(name: "scope", value: scopes),
            .init(name: "code_challenge_method", value: "S256"), .init(name: "code_challenge", value: challenge),
            .init(name: "state", value: state),
        ]
        NSWorkspace.shared.open(url.url!)
    }

    // The approval code for a refresh token (Spotify's token endpoint, PKCE)
    private static func exchange(_ code: String, _ verifier: String, _ done: @escaping (Result<String, Error>) -> Void) {
        var req = URLRequest(url: URL(string: "https://accounts.spotify.com/api/token")!)
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        var form = URLComponents()
        form.queryItems = [
            .init(name: "grant_type", value: "authorization_code"), .init(name: "code", value: code),
            .init(name: "redirect_uri", value: redirect), .init(name: "client_id", value: clientID),
            .init(name: "code_verifier", value: verifier),
        ]
        req.httpBody = form.percentEncodedQuery?.data(using: .utf8)
        URLSession.shared.dataTask(with: req) { data, _, error in
            let token = data.flatMap { (try? JSONSerialization.jsonObject(with: $0)) as? [String: Any] }?["refresh_token"] as? String
            DispatchQueue.main.async {
                if let token { done(.success(token)) } else { done(.failure(error ?? Failure.noToken)) }
            }
        }.resume()
    }

    private static func reply(_ c: NWConnection, _ message: String) {
        let html = "<html><body style='font:20px -apple-system;padding:48px'>\(message)</body></html>"
        let resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: \(html.utf8.count)\r\nConnection: close\r\n\r\n\(html)"
        c.send(content: Data(resp.utf8), completion: .contentProcessed { _ in c.cancel() })
    }

    private static func randomURLSafe(_ bytes: Int) -> String {
        var b = [UInt8](repeating: 0, count: bytes)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes, &b)
        return Data(b).base64URL
    }
}

private extension Data {
    var base64URL: String {
        base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}
