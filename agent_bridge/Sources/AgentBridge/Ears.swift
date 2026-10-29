// Hearing what you say to the board: its microphones' sound (16 kHz, echo-cancelled on the board) through macOS's
// speech recognition, on this Mac. Words come as they're recognised (partial), and once you pause, the whole thing
// (an utterance): that's your turn. Each utterance is a fresh recognition request (they can't run for long).
import AVFoundation
import Speech

final class Ears {
    var onPartial: (String) -> Void = { _ in }
    var onUtterance: (String) -> Void = { _ in }
    var pause: TimeInterval = 0.9                     // this long without a new word: you've finished

    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "en-US"))
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var text = ""
    private var changed = Date()
    private var firstWord: Date?                       // when this utterance's first word came
    private var begun = Date()
    private var timer: Timer?
    private let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false)!
    private(set) var listening = false

    static var allowed: Bool { SFSpeechRecognizer.authorizationStatus() == .authorized }
    static func askPermission(_ done: @escaping (Bool) -> Void) {
        SFSpeechRecognizer.requestAuthorization { s in DispatchQueue.main.async { done(s == .authorized) } }
    }

    func start() {
        guard !listening else { return }
        listening = true
        begin()
        let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)         // (keeps going while the menu's open)
        timer = t
    }

    func stop() {
        listening = false
        timer?.invalidate()
        timer = nil
        end()
        text = ""
    }

    // Forget what's been heard so far (it was the board's own voice, or it's been dealt with)
    func reset() {
        guard listening else { return }
        end()
        text = ""
        begin()
    }

    var heard: String { text }

    func feed(_ pcm: Data) {
        guard listening, let request else { return }
        let n = pcm.count / 2
        guard n > 0, let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(n)) else { return }
        buf.frameLength = AVAudioFrameCount(n)
        let out = buf.floatChannelData![0]
        pcm.withUnsafeBytes { raw in
            let s = raw.bindMemory(to: Int16.self)
            for i in 0..<n { out[i] = Float(Int16(littleEndian: s[i])) / 32768 }
        }
        request.append(buf)
    }

    private func begin() {
        firstWord = nil
        guard let recognizer, recognizer.isAvailable else { return }
        let r = SFSpeechAudioBufferRecognitionRequest()
        r.shouldReportPartialResults = true
        r.taskHint = .dictation
        r.addsPunctuation = true
        if recognizer.supportsOnDeviceRecognition { r.requiresOnDeviceRecognition = true }
        request = r
        begun = Date()
        text = ""
        task = recognizer.recognitionTask(with: r) { [weak self, weak r] result, _ in
            guard let self, let r, self.request === r, let result else { return }
            let t = result.bestTranscription.formattedString
            if t != self.text {
                if self.text.isEmpty { self.firstWord = Date() }
                self.text = t
                self.changed = Date()
                self.onPartial(t)
            }
        }
    }

    private func end() {
        request?.endAudio()
        task?.cancel()
        request = nil
        task = nil
    }

    private func tick() {
        guard listening else { return }
        let t = text.trimmingCharacters(in: .whitespaces)
        // you've paused; or it's gone on so long without one that something else is talking too (a TV, music): take it
        if !t.isEmpty, Date().timeIntervalSince(changed) > pause || Date().timeIntervalSince(firstWord ?? Date()) > 20 {
            reset()
            onUtterance(t)
        } else if t.isEmpty, Date().timeIntervalSince(begun) > 50 || request == nil {
            reset()                                   // (a request only lasts about a minute: start another quietly)
        }
    }
}
