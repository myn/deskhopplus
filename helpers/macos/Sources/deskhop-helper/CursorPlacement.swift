import CoreGraphics
import DHCore

final class CursorPlacement {
    var log: (String) -> Void = { _ in }
    private var lastChainDirection: UInt8?

    private struct DisplaySnapshot {
        let ids: [CGDirectDisplayID]
        let rects: [dh_display_rect]
        let primary: Int
    }

    private func displays() -> DisplaySnapshot? {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return nil }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetActiveDisplayList(count, &ids, &count) == .success else { return nil }
        ids = Array(ids.prefix(Int(count)))
        let rects = ids.map { id -> dh_display_rect in
            let frame = CGDisplayBounds(id)
            return dh_display_rect(x: Int32(frame.origin.x.rounded()),
                                   y: Int32(frame.origin.y.rounded()),
                                   width: Int32(frame.width.rounded()),
                                   height: Int32(frame.height.rounded()))
        }
        return DisplaySnapshot(ids: ids, rects: rects,
                               primary: ids.firstIndex(of: CGMainDisplayID()) ?? 0)
    }

    func received(type: UInt8, body: [UInt8]) -> Bool {
        guard type == UInt8(DH_MSG_PLACE.rawValue) else { return false }

        var place = dh_place()
        let decoded = body.withUnsafeBufferPointer {
            dh_place_decode($0.baseAddress, $0.count, &place)
        }
        guard decoded else {
            log("ignored a malformed cursor placement")
            return true
        }
        lastChainDirection = place.chain_direction

        guard let displays = displays() else {
            log("could not enumerate displays for cursor placement")
            return true
        }
        var target = dh_place_point()
        let found = displays.rects.withUnsafeBufferPointer {
            dh_place_target(&place, $0.baseAddress, $0.count, displays.primary, &target)
        }
        guard found else {
            log("cursor placement named a monitor outside the configured display chain")
            return true
        }

        let result = CGWarpMouseCursorPosition(CGPoint(x: Int(target.x), y: Int(target.y)))
        if result != .success {
            log("cursor placement was refused (CoreGraphics error \(result.rawValue))")
        }
        return true
    }

    func positionBody() -> [UInt8]? {
        guard let chain = lastChainDirection,
              let event = CGEvent(source: nil) else { return nil }
        guard let displays = displays() else { return nil }
        let cursor = event.location
        guard let displayIndex = displays.ids.firstIndex(
            where: { CGDisplayBounds($0).contains(cursor) })
        else { return nil }

        var chainIndex: UInt8?
        for index in 1...min(displays.rects.count, 255) {
            var request = dh_place(chain_index: UInt8(index), chain_direction: chain,
                                   border_direction: 4, entry_position: 0)
            var point = dh_place_point()
            let found = displays.rects.withUnsafeBufferPointer {
                dh_place_target(&request, $0.baseAddress, $0.count, displays.primary, &point)
            }
            if found && point.display_index == displayIndex {
                chainIndex = UInt8(index)
                break
            }
        }
        guard let chainIndex else { return nil }
        let frame = CGDisplayBounds(displays.ids[displayIndex])
        let x = UInt16(clamping: Int(((cursor.x - frame.minX) / max(frame.width - 1, 1)) * 65535))
        let y = UInt16(clamping: Int(((cursor.y - frame.minY) / max(frame.height - 1, 1)) * 65535))
        var position = dh_position(chain_index: chainIndex, x: x, y: y)
        var body = [UInt8](repeating: 0, count: Int(DH_POSITION_BODY_SIZE))
        let encoded = body.withUnsafeMutableBufferPointer {
            dh_position_encode(&position, $0.baseAddress, $0.count)
        }
        return encoded ? body : nil
    }
}
