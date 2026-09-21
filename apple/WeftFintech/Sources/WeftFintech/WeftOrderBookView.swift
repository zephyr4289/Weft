// WeftOrderBookView.swift — SwiftUI depth ladder over WeftOrderBookModel.
//
// TimelineView(.animation) drives the redraw cadence; the Canvas draw
// closure is GEOMETRY-ONLY (reads model.levels scratch, one reused
// GraphicsContext state, zero allocations per frame). Labels are OPT-IN
// (`showLabels: true`) and documented as allocating — the same Pillar 4
// trade-off as the Flutter painter.

import SwiftUI

public struct WeftOrderBookView: View {
    @State private var model = WeftOrderBookModel()
    private let feed: () -> UnsafeRawBufferPointer?
    private let showLabels: Bool

    public init(showLabels: Bool = false,
                feed: @escaping () -> UnsafeRawBufferPointer?) {
        self.showLabels = showLabels
        self.feed = feed
    }

    public var body: some View {
        TimelineView(.animation) { context in
            Canvas { ctx, size in
                if let record = feed() {
                    _ = model.ingest(record)
                }
                draw(ctx: ctx, size: size, date: context.date)
            }
        }
    }

    private func draw(ctx: GraphicsContext, size: CGSize, date: Date) {
        guard model.bookValid else {
            // FALLBACK band: fail-closed, never throws.
            var warn = Path()
            warn.addRect(CGRect(x: 0, y: 0, width: size.width, height: 6))
            ctx.fill(warn, with: .color(Color.red.opacity(0.4)))
            return
        }
        let gutter = size.width * 0.5
        let rowH = size.height / CGFloat(Mdp1.topLevels * 2)
        var maxSize: UInt32 = 1
        for i in 0..<Mdp1.topLevels {
            maxSize = max(maxSize, model.levels[i * 3 + 1])
            maxSize = max(maxSize, model.levels[(Mdp1.topLevels + i) * 3 + 1])
        }
        let maxF = CGFloat(maxSize)

        for i in 0..<Mdp1.topLevels {
            // bid row i: grows left from the gutter
            let b = i * 3
            let fracB = CGFloat(model.levels[b + 1]) / maxF
            let wB = (gutter - 24) * fracB
            let rectB = CGRect(x: gutter - wB,
                               y: size.height * 0.5 - CGFloat(i + 1) * rowH,
                               width: wB,
                               height: rowH - 1)
            ctx.fill(Path(roundedRect: rectB, cornerRadius: 2),
                     with: .color(Color.green.opacity(0.15 + 0.75 * fracB)))

            // ask row i: grows right from the gutter
            let a = (Mdp1.topLevels + i) * 3
            let fracA = CGFloat(model.levels[a + 1]) / maxF
            let wA = (gutter - 24) * fracA
            let rectA = CGRect(x: gutter,
                               y: size.height * 0.5 + CGFloat(i) * rowH,
                               width: wA,
                               height: rowH - 1)
            ctx.fill(Path(roundedRect: rectA, cornerRadius: 2),
                     with: .color(Color.red.opacity(0.15 + 0.75 * fracA)))
        }

        // spread indicator
        let hot: Color = model.locked ? .yellow : model.crossed ? .red : .white
        ctx.fill(Path(CGRect(x: gutter - 1, y: 0, width: 2, height: size.height)),
                 with: .color(hot.opacity(0.2)))

        if showLabels {
            // OPT-IN label path — Text allocation per frame by design.
            ctx.draw(Text(String(model.bestBid)),
                     at: CGPoint(x: 44, y: size.height * 0.5 - rowH * 0.5))
            ctx.draw(Text(String(model.bestAsk)),
                     at: CGPoint(x: size.width - 44,
                                 y: size.height * 0.5 + rowH * 0.5))
        }
    }
}
