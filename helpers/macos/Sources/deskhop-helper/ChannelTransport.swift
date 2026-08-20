import DeskhopChannel
import Foundation
import IOKit
import IOKit.hid

/*
 * The IOKit end of the channel: find the device, seize every channel or none,
 * carry reports in and out. It decides nothing — it reports what it sees to
 * the session and does what the session asks (HelperSession.swift, which is
 * itself only a binding onto the shared core's machine).
 *
 * Platform-boundary code, verified by hand and by use rather than at a seam
 * (#42, "Not tested at a seam").
 */
final class ChannelTransport {
    /// Events for the session.
    var onEvent: ((SessionInput) -> Void)?
    /// Diagnostics.
    var log: ((String) -> Void)?

    private final class Channel {
        let device: IOHIDDevice
        /* IOKit writes input reports into this buffer for the lifetime of the
           callback registration, so it outlives every call. */
        let buffer: UnsafeMutablePointer<UInt8>
        var opened = false

        init(device: IOHIDDevice) {
            self.device = device
            self.buffer = .allocate(capacity: ChannelIdentity.reportSize)
            buffer.initialize(repeating: 0, count: ChannelIdentity.reportSize)
        }

        deinit { buffer.deallocate() }
    }

    private let manager: IOHIDManager
    private var channels: [Channel] = []
    private var configModeNodes = 0

    /*
     * The serial of the device this helper is talking to. Every channel must
     * belong to it: the identity is the USB identifier and serial, never a
     * device path, which on neither platform survives a reconnect.
     */
    private(set) var serial: String?

    init() {
        manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    // MARK: - Discovery

    func start() {
        /*
         * Match narrowly: the vendor usage page and the channel's own usage,
         * plus the identifier. Broad matching would open a keyboard and
         * trigger an Input Monitoring prompt — the mistake behind most public
         * claims that HID access requires one (ADR-0001).
         *
         * The config-mode identity is matched too, on its own vendor
         * collection: seeing it is how the helper knows the device rebooted
         * into config mode rather than being unplugged.
         */
        let normal: [String: Any] = [
            kIOHIDVendorIDKey: ChannelIdentity.vendorID,
            kIOHIDProductIDKey: ChannelIdentity.productID,
            kIOHIDDeviceUsagePageKey: ChannelIdentity.usagePage,
            kIOHIDDeviceUsageKey: ChannelIdentity.usage,
        ]
        let configMode: [String: Any] = [
            kIOHIDVendorIDKey: ChannelIdentity.configVendorID,
            kIOHIDProductIDKey: ChannelIdentity.configProductID,
            kIOHIDDeviceUsagePageKey: ChannelIdentity.usagePage,
        ]
        IOHIDManagerSetDeviceMatchingMultiple(manager, [normal, configMode] as CFArray)

        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<ChannelTransport>.fromOpaque(context).takeUnretainedValue().matched(device)
        }, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<ChannelTransport>.fromOpaque(context).takeUnretainedValue().removed(device)
        }, context)

        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
        IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    private func matched(_ device: IOHIDDevice) {
        guard identity(of: device) == .normal else {
            configModeNodes += 1
            onEvent?(.deviceAppeared(.configMode))
            return
        }

        let deviceSerial = property(device, kIOHIDSerialNumberKey) as? String
        if let known = serial, let deviceSerial, known != deviceSerial {
            /* Behaviour with more than one device attached is out of scope
               (#42); the first serial seen wins, and the rest is ignored
               rather than silently mixed into one session. */
            log?("ignoring a second device with serial \(deviceSerial); holding \(known)")
            return
        }
        serial = deviceSerial ?? serial

        channels.append(Channel(device: device))
        log?("channel found on serial \(serial ?? "(none exposed)"): \(channels.count) so far")
        onEvent?(.deviceAppeared(.normal))
    }

    private func removed(_ device: IOHIDDevice) {
        if identity(of: device) == .configMode {
            configModeNodes = max(0, configModeNodes - 1)
            /* Unplugged while in config mode: the device is now absent, not
               configuring, and saying nothing would leave "Device in config
               mode" showing for as long as it stays unplugged. */
            if configModeNodes == 0 && channels.isEmpty {
                onEvent?(.deviceDisappeared)
            }
            return
        }

        /* Unregister before the channel is dropped: the callback holds the
           buffer the channel owns, and the channel deallocates it. */
        for channel in channels where channel.device == device {
            unlisten(channel)
        }
        channels.removeAll { $0.device == device }

        if channels.isEmpty {
            serial = nil
            onEvent?(.deviceDisappeared)
        }
    }

    private enum Identity { case normal, configMode }

    private func identity(of device: IOHIDDevice) -> Identity {
        let vendor = property(device, kIOHIDVendorIDKey) as? Int
        return vendor == ChannelIdentity.configVendorID ? .configMode : .normal
    }

    private func property(_ device: IOHIDDevice, _ key: String) -> Any? {
        IOHIDDeviceGetProperty(device, key as CFString)
    }

    // MARK: - Exclusivity

    /*
     * Seize every channel or none (ADR-0002). Partial acquisition is the
     * dangerous state, not the tolerable one: a second process holding one
     * channel would silently receive part of every bulk transfer while both
     * sides looked healthy — so anything short of all is rolled back and
     * reported as a refusal.
     */
    func acquire() {
        guard !channels.isEmpty else { return }
        /* Nothing to do when every channel is already held. With more than one
           channel (#63) the nodes arrive one at a time, so this runs again as
           each turns up and the session is re-established on the full set. */
        guard channels.contains(where: { !$0.opened }) else { return }

        var acquired = 0
        for channel in channels {
            if channel.opened {
                acquired += 1
                continue
            }
            let result = IOHIDDeviceOpen(channel.device,
                                         IOOptionBits(kIOHIDOptionsTypeSeizeDevice))
            guard result == kIOReturnSuccess else {
                log?("exclusive open refused: \(String(format: "0x%08x", result))")
                break
            }
            channel.opened = true
            listen(to: channel)
            acquired += 1
        }

        guard acquired == channels.count else {
            release()
            onEvent?(.acquisitionRefused(acquired: acquired, of: channels.count))
            return
        }

        log?("holding \(acquired) channel(s) exclusively")
        onEvent?(.channelsAcquired(count: acquired))
    }

    func release() {
        for channel in channels where channel.opened {
            unlisten(channel)
            IOHIDDeviceClose(channel.device, IOOptionBits(kIOHIDOptionsTypeSeizeDevice))
            channel.opened = false
        }
    }

    private func unlisten(_ channel: Channel) {
        guard channel.opened else { return }
        IOHIDDeviceRegisterInputReportCallback(channel.device, channel.buffer,
                                               ChannelIdentity.reportSize, nil, nil)
    }

    private func listen(to channel: Channel) {
        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDDeviceRegisterInputReportCallback(
            channel.device, channel.buffer, ChannelIdentity.reportSize,
            { context, _, _, _, _, report, length in
                guard let context, length > 0 else { return }
                let transport = Unmanaged<ChannelTransport>.fromOpaque(context)
                    .takeUnretainedValue()
                transport.onEvent?(.received(Array(UnsafeBufferPointer(start: report,
                                                                      count: Int(length)))))
            }, context)
    }

    // MARK: - Writing

    /*
     * Session and control traffic goes on channel 0; bulk striping across the
     * rest is #47's and arrives with the relay. A report is a fixed 64 bytes
     * with a padded tail, since it carries no length of its own.
     */
    func send(_ frameBytes: [UInt8]) {
        guard let channel = channels.first, channel.opened else {
            log?("dropped \(frameBytes.count) bytes: no channel held")
            return
        }

        let frame: Frame
        do {
            frame = try FrameCodec.decode(frameBytes).frame
        } catch {
            log?("refusing to send bytes that are not a frame: \(error)")
            return
        }

        guard let reports = try? FrameCodec.reports(for: [frame]) else { return }
        for report in reports {
            let result = report.withUnsafeBufferPointer { buffer in
                IOHIDDeviceSetReport(channel.device, kIOHIDReportTypeOutput, 0,
                                     buffer.baseAddress!, buffer.count)
            }
            if result != kIOReturnSuccess {
                /* A frame written in part leaves the device's reader holding
                   half of one, where the padding skip does not apply — the
                   next frame would be eaten as its tail. The connection goes. */
                onEvent?(.transportFailed("report write failed: "
                                          + String(format: "0x%08x", result)))
                return
            }
        }
    }

    var isHoldingChannels: Bool { channels.contains { $0.opened } }
    var hasDevice: Bool { !channels.isEmpty }
}
