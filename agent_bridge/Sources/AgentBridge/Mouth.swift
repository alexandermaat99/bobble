// Claude's voice: each sentence spoken by macOS's speech synthesizer into audio (not out of the Mac's speaker), turned
// into 16 kHz 16-bit mono for the board. Sentences are said in turn; stop() drops whatever's left.
import AVFoundation

final class Mouth {
    var onAudio: (Data, String?) -> Void = { _, _ in }   // audio, and the sentence it starts (with its first piece)

    private let synth = AVSpeechSynthesizer()
    private var queue: [String] = []
    private var busy = false
    private var generation = 0
    private var sentenceNo = 0                       // (for the watchdog: which sentence is being made)
    private var converter: AVAudioConverter?
    private var from: AVAudioFormat?
    private let out = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 16000, channels: 1, interleaved: true)!
    var voice: AVSpeechSynthesisVoice? = Mouth.bestVoice()

    var idle: Bool { !busy && queue.isEmpty }

    // The nicest English voice on this Mac (a Premium or Enhanced one, if it's been downloaded in System Settings ›
    // Accessibility › Spoken Content), or the one chosen with `defaults write app.bobble.Bobble chatVoice <identifier>`
    static func bestVoice() -> AVSpeechSynthesisVoice? {
        if let id = UserDefaults.standard.string(forKey: "chatVoice"), let v = AVSpeechSynthesisVoice(identifier: id) { return v }
        let lang = Locale.current.language.languageCode?.identifier == "en" ? Locale.current.identifier.replacingOccurrences(of: "_", with: "-") : "en-US"
        let voices = AVSpeechSynthesisVoice.speechVoices().filter { v in
            guard v.language.hasPrefix("en") else { return false }
            if #available(macOS 14, *) { return !v.voiceTraits.contains(.isNoveltyVoice) }
            return true
        }
        func rank(_ v: AVSpeechSynthesisVoice) -> Int {
            (v.quality == .premium ? 20 : v.quality == .enhanced ? 10 : 0) + (v.language == lang ? 2 : 0) + (v.language == "en-US" ? 1 : 0)
        }
        return voices.max { rank($0) < rank($1) } ?? AVSpeechSynthesisVoice(language: "en-US")
    }

    func say(_ sentence: String) {
        let s = sentence.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return }
        queue.append(s)
        next()
    }

    func stop() {
        generation += 1
        queue.removeAll()
        if busy { synth.stopSpeaking(at: .immediate) }
        busy = false
    }

    private func next() {
        guard !busy else { return }
        guard !queue.isEmpty else { return }
        let s = queue.removeFirst()
        busy = true
        let gen = generation
        sentenceNo += 1
        let no = sentenceNo
        // (a voice that never says it's finished mustn't hold everything up: give up on the sentence after a while)
        DispatchQueue.main.asyncAfter(deadline: .now() + 6 + Double(s.count) * 0.08) { [weak self] in
            guard let self, gen == self.generation, self.busy, self.sentenceNo == no else { return }
            NSLog("voice chat: the speech synthesizer never finished a sentence; moving on")
            self.busy = false
            self.next()
        }
        let u = AVSpeechUtterance(string: s)
        u.voice = voice
        u.rate = AVSpeechUtteranceDefaultSpeechRate * 1.04
        var first = true
        synth.write(u) { [weak self] buffer in
            guard let pcm = buffer as? AVAudioPCMBuffer else { return }
            let data = pcm.frameLength > 0 ? self?.convert(pcm) : nil
            DispatchQueue.main.async {
                guard let self, gen == self.generation, no == self.sentenceNo else { return }
                if let data, !data.isEmpty {
                    self.onAudio(data, first ? s : nil)
                    first = false
                }
                if pcm.frameLength == 0 {                // (the end of this sentence)
                    self.busy = false
                    self.next()
                }
            }
        }
    }

    // (called on the synthesizer's own thread)
    private func convert(_ b: AVAudioPCMBuffer) -> Data? {
        if from != b.format || converter == nil {
            from = b.format
            converter = AVAudioConverter(from: b.format, to: out)
        }
        guard let converter else { return nil }
        let cap = AVAudioFrameCount(Double(b.frameLength) * 16000 / b.format.sampleRate) + 64
        guard let o = AVAudioPCMBuffer(pcmFormat: out, frameCapacity: cap) else { return nil }
        var fed = false
        var err: NSError?
        converter.convert(to: o, error: &err) { _, status in
            if fed { status.pointee = .noDataNow; return nil }
            fed = true
            status.pointee = .haveData
            return b
        }
        guard err == nil, o.frameLength > 0, let p = o.int16ChannelData else { return nil }
        return Data(bytes: p[0], count: Int(o.frameLength) * 2)
    }
}
