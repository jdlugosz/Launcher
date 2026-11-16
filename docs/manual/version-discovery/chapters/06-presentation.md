# 6. A coherent icon presentation

A discovered path is not yet a finished icon. Preparing its label can read executable metadata; finding an application can consult the registry; loading artwork can enter a Shell provider or decode an image. Those operations may be slow independently of directory discovery. Launcher gives them a separate worker stage, then adopts the resulting action and appearance as one coherent presentation.

@figure presentation | Discovery chooses a catalog; preparation selects an exact action and produces owned pixels; the UI adopts the coherent generation.

## Request, result, and apartment ownership

`ItemPresentationRequest` owns the configured item, selected concrete action, artwork source, configuration directory, and requested physical pixel size. For a versioned item, a missing action means unresolved. The adapter deliberately refuses to rediscover the item's original wildcard or Adobe selector. That prevents a second scan hidden inside image preparation and keeps the result tied to its chosen catalog.

The result contains a prepared launch, label, cached administrator/reveal capabilities, diagnostics, and tightly packed premultiplied BGRA pixels. The preparation worker creates its private STA for imaging and Shell work, copies the decoded pixels, and destroys those COM owners before returning. No Shell, WIC, or Direct2D interface is transferred to the UI apartment.

@source include/launcher/item_presentation.hpp | struct ItemPresentationRequest final
@source include/launcher/item_presentation.hpp | struct ItemPresentation final
@source src/item_presentation.cpp | ItemPresentation prepare_item_presentation

The UI reconstructs its own WIC bitmap from the owned bytes and creates a local Direct2D bitmap when required. This copying boundary costs memory, but makes the thread and apartment contract explicit. Image failure does not veto an otherwise valid launch: the worker attempts a stock fallback, and the UI can retain embedded provisional artwork. Explicit labels and artwork remain configuration choices.

## Generations prevent mixed revisions

Each binding has desired, running, and adopted preparation jobs. A job records its generation, shown-page visit, concrete request, and exact catalog snapshot. When a newer request supersedes older work, that old worker can finish safely, but its generation does not become the binding's desired result.

@source src/main.cpp | struct ItemPresentationRecord final
@source src/main.cpp | void queue_item_presentation

On adoption, Launcher changes the icon's prepared launch, label, catalog, capability flags, tooltip, pixels, and generation together. Thus a menu opened from the icon uses the catalog associated with its displayed file, even if the discovery service already has a newer result. The application never pairs an old executable's artwork with a newly selected default merely because directory discovery finished first.

@code src/main.cpp | icon.launch = value.launch; | 8 | The action and its associated presentation facts are installed together.

Adoption waits while a menu, mouse press, drag, gesture dispatch, rebuild, or other protected transition owns the current view. Once ordinary dispatch returns, the application reconsiders ready work. If newly prepared pixels equal those already displayed, the existing image can be reused while freshness and other presentation facts advance. The first visible visit can join startup preparation; later visits refresh file-sensitive data once per visit, rather than on each hover or repaint.

@source src/main.cpp | bool version_adoption_held() const noexcept
@source src/main.cpp | void adopt_item_presentations()

## Bounded preparation and honest memory accounting

Presentation admission permits four jobs at once and at most 64 MiB of admitted output pixels. Individual output dimensions must be within 1-4096 pixels. The active page also divides a 64 MiB decoded-pixel allowance among its icons; retained inactive presentation pixels have a separate 64 MiB cache limit and can be discarded.

These are explicit accounting limits, not a promise about total process working set. WIC and Direct2D copies, decoder internals, provider allocations, thread stacks, and allocator overhead exist in addition. A Shell provider can also hold a preparation worker for an arbitrary time. Finite admission contains that growth but cannot make an unresponsive provider return.

@source src/main.cpp | constexpr std::size_t active_limit = 4;
@source src/main.cpp | void trim_item_presentations()

## The attention-only blinds

The pending overlay uses embedded SVG variants: four slats at an effective logical icon extent up to 32 DIP, five through 64 DIP, and six above that. Fit scaling changes that logical extent. DPI changes the physical raster dimensions, so raising DPI alone does not add slats. The raster cache is keyed by physical size and selected density.

@source src/pending_overlay.cpp | unsigned pending_overlay_slats
@source src/main.cpp | void prepare_pending_overlay

The predicate requires the pointer over this icon and its adopted search state queued or running. Transparent gaps leave the underlying artwork visible, and the overlay changes neither hit testing nor its action. When the attempt ends, an invalidation removes the overlay even if the catalog was unchanged or refresh failed. There is no animation or idle repaint timer: a slow drive can leave the static pattern visible for as long as attention remains there.

@source src/main.cpp | bool pending_overlay_visible
@source src/main.cpp | // Pending artwork must disappear on an unchanged or failed scan,

@figure overlay | Three production-rendered examples show the density thresholds. The [preview instructions](../../../preview-images.md) show how to generate other sizes, backgrounds and native display scales.
