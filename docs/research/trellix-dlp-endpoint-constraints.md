# Trellix DLP Endpoint constraints for an unsigned, unelevated Windows helper

Research for [issue #57](https://github.com/myn/deskhopplus/issues/57), under
[map #31](https://github.com/myn/deskhopplus/issues/31). Corrects the vendor framing of
[`windows-helper-constraints.md`](./windows-helper-constraints.md) (issue
[#36](https://github.com/myn/deskhopplus/issues/36)), which analysed Microsoft Purview / Endpoint
DLP — a product that [#40](https://github.com/myn/deskhopplus/issues/40) measured as **not running**
on the target machine.

**Question.** The enforcing endpoint agent on the managed work laptop is **Trellix DLP Endpoint**
(formerly McAfee DLP Endpoint), service `TrellixDLPAgentService`, kernel drivers `hdlpctrl`,
`hdlpevnt`, `hdlpflt`, `hdlphook`, `hdlpnetmonitor`. Given that:

1. Does Trellix DLP intercept clipboard access by an unsigned third-party process, at what level,
   on what criteria, and what does a block look like from the calling application?
2. Does Trellix Device Control treat a USB CDC or USB composite device as in scope, and can it stop
   `usbser.sys` from binding the way Windows Device Installation Restrictions would?
3. Can a standard user, without admin rights, determine which Trellix policies are applied and
   whether Trellix blocked something?
4. Which product versions do the documented behaviours and the `hdlp*` driver names correspond to?

**Scope note.** This is a documentation exercise about *what the product can be configured to do*.
It cannot tell us what the local Trellix administrators have actually configured — that is policy
held in an ePO console we have no access to. The value here is (a) establishing the rule vocabulary,
so the empirical checks ask the right questions, and (b) one structural finding about which driver
is missing from the machine, which narrows the device-control risk considerably.

**Sourcing.** Every Trellix claim below cites `docs.trellix.com`, predominantly the
*Trellix Data Loss Prevention 11.14.x Product Guide* (the current on-premises guide), with
version-specific variants noted where the wording changed. Windows-mechanism claims cite Microsoft
Learn. Where a conclusion is reasoned from primary sources rather than stated by them it is marked
**[INFERENCE]**; where no primary source could be found it is marked **[UNVERIFIED]**.

Two sourcing limitations, stated rather than papered over:

- **`kcm.trellix.com` (the Trellix Knowledge Center) was unreachable** from this environment —
  connections to `38.109.230.120:443` were refused, repeatedly. KB articles that search engines
  surface as relevant, notably **KB85654 "Explanation of the clipboard blocking feature"** and
  **KB89301 "Trellix Data Loss Prevention 11.x.x Known Issues"**, could not be read. Anything those
  articles might settle is left in the open-questions section rather than guessed at.
- `docs.trellix.com` renders client-side and returns an empty shell to a plain fetch. The content
  was read from the portal's own prerender backend, `docs-be.trellix.com`, at identical paths.
  Citations below use the public `docs.trellix.com` URLs, which serve the same topics to a browser.

---

## Summary of findings

| Question | Answer |
| --- | --- |
| Can a clipboard rule block **unclassified** content? | **Yes.** `Classification` accepts `is any data (ALL)`, and the documented ban on combining `ALL` with `Block` applies **only** to Application File Access Protection rules, not to Clipboard Protection. The "our content was unlabeled, so we're safe" reading of #40 does **not** hold. |
| What can a clipboard rule discriminate on? | Classification, End-User group, **Source application** (paste-from), **Destination application** (paste-into), each defaulting to `ALL`. Applications are matched by executable name/path/hash/vendor/product/command line/window title — an unsigned third-party helper is trivially expressible as a destination. |
| Available clipboard actions | `No Action` (monitor), **`Block`**, **`Request justification`**, plus `Report Incident`, `User Notification`, `Store original file as evidence`. |
| What a block looks like to the caller | **[UNVERIFIED].** Trellix documents only "Blocks the action" and an optional end-user pop-up. Nothing primary describes the Win32-level result (error vs empty vs substituted data). |
| Which driver does clipboard interception | **[INFERENCE]** `hdlphook` ("Hook driver"). Trellix documents the name and that it should be running, but **never says what it hooks**. The claim in #57 that `hdlphook` is "the clipboard / screen-capture interception component" is not backed by any primary source found. |
| Is a USB CDC device "removable storage"? | **No.** Removable-storage rules trigger "when a new file system is mounted". A CDC/serial device mounts no file system. |
| Can Trellix block the device at all? | **Yes, via a plug-and-play device rule** — matching on Bus Type, Device Class, USB Class Code, USB VID/PID, Device Instance Path, or Compatible IDs. Reaction is `Block` or `No Action` only. |
| **Is plug-and-play blocking live on this machine?** | ~~**Almost certainly not.**~~ **RETIRED BY MEASUREMENT — see the correction below.** `hdlpdbk` is registered, on disk, and is a **registered class filter on Ports, USBDevice and USB**. |
| Does it block driver *installation*? | **No.** It is a device-class filter driver, i.e. it acts on the device stack at plug-in, not on driver installation. Distinct from Windows `DeviceInstall\Restrictions`, which #40 found entirely unconfigured. |
| Observability without admin | **Partial.** The `DLP Endpoint Console` (`DlpConsoleRunner.exe`) shows notification history and active-policy revision, if the admin enabled the client UI. The full `hdlpDiag.exe` diagnostic tool **requires a Help-Desk-issued release code** and is not available to a standard user. |
| Local log/policy-cache paths | **[UNVERIFIED].** No Trellix document found states where the Windows client writes logs or caches policy. |

**The one line that changes the risk picture:** the clipboard risk is *higher* than #40 implied
(unlabeled content is not exempt by design), and ~~the device risk is *lower* than #57 feared (the
driver that enforces plug-and-play device rules is not loaded)~~ — **see the correction below; the
device conclusion was wrong.**

---

## CORRECTION (measured 2026-08-09, Part F of `tools/windows-checks/Confirm-Check1.ps1`)

**The device-control conclusion in this document was wrong, and the reasoning that produced it was
unsound.** It inferred "not deployed" from `hdlpdbk`'s absence from #40's driver list — but that list
was produced by a query filtered to `State = Running`. Absence from it meant "not currently loaded",
never "not installed". Measurement:

```
service hdlpdbk   State=Stopped  StartMode=Manual
file    hdlpdbk.sys   54616 bytes  ver=12.11.11.3
```

And the device-class filter registry — the check this document itself nominated as the highest-value
read — shows Trellix registered as a filter on **every class our device travels except HIDClass**:

| Class | GUID | Filter |
| --- | --- | --- |
| Ports (COM & LPT) — where a `usbser.sys` CDC interface lands | `4d36e978-…` | `LowerFilters: hdlpdbk` |
| USBDevice | `88BAE032-…` | `UpperFilters: hdlpdbk` |
| USB (system) — the composite parent | `36fc9e60-…` | `UpperFilters: hdlpdbk` |
| HIDClass | `745a17a0-…` | *(none)* |

`State=Stopped` with `StartMode=Manual` is the **normal** resting state for a PnP class filter with no
matching device attached; it is not evidence the module is disabled. The correct statement is that
plug-and-play device blocking is **configured into the device stack** for the relevant classes, and
whether it attaches and enforces when our device arrives is unresolved until the device exists.

**Corroborating evidence that device rules are live**, from the DLP Endpoint Console's notification
history on the machine: it lists an entry for **every occasion the DeskHop device was put into
web-config mode**, where it enumerates as USB mass storage. So removable-storage rules are not merely
configured, they are matching this specific hardware and raising user-visible notifications — which in
Trellix's vocabulary accompanies `Block` or `Request justification`, not silent monitoring.

This does *not* contradict §3.2: those rules trigger on a mounted file system, which web-config mode
provides and a CDC interface does not. It does mean the mass-storage web-config path is
policy-visible on managed machines, independent of the clipboard and cursor questions.

**Lesson for the rest of this document:** "absent from a measurement" is only as strong as the
measurement's filter. Two other conclusions here rest on similar negative evidence and deserve the
same scepticism — the `[UNVERIFIED]` local-artefact paths in §4.3 (since disproved: see the
correction note there) and any claim resting on a documentation search returning zero hits.

---

## 1. What is installed, and what each `hdlp*` driver is documented to do

Trellix publishes the authoritative mapping in the Diagnostic Tool chapter, as the table of what a
healthy agent looks like:

> **Agent processes**
>
> | Term | Process | Expected status |
> | --- | --- | --- |
> | Fcag | Trellix DLP Endpoint agent (client) | enabled; running |
> | Fcags | Trellix DLP Endpoint agent service | enabled; running |
> | Fcagte | Trellix DLP Endpoint text extractor | enabled; running |
> | Fcagwd | Trellix DLP Endpoint watch dog | enabled; running |
> | Fcagd | Trellix DLP Endpoint agent with automatic dump | enabled only for troubleshooting. |
>
> **Drivers**
>
> | Term | Process | Expected status |
> | --- | --- | --- |
> | Hdlpflt | Trellix DLP Endpoint minifilter driver (enforces removable storage device rules) | enabled; running |
> | Hdlpevnt | Trellix DLP Endpoint event | enabled; running |
> | Hdlpdbk | Trellix DLP Endpoint device filter driver (enforces device Plug and Play rules) | can be disabled in configuration |
> | Hdlpctrl | Trellix DLP Endpoint control | enabled; running |
> | Hdlhook | Trellix DLP Endpoint Hook driver | enabled; running |

— [Checking the agent status](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-e196c74d-9149-c083-3f4d-2028b184ab44.html)
("Hdlhook" is Trellix's typo for `hdlphook`; the same typo is present in every version of this page
from 11.1.x through 11.14.x.)

Cross-referencing that table against the drivers measured on the machine in
[#40](https://github.com/myn/deskhopplus/issues/40):

| Driver | Documented role | Present on the machine? |
| --- | --- | --- |
| `hdlpctrl` | "control" (no further detail) | yes |
| `hdlpevnt` | "event" (no further detail) | yes |
| `hdlpflt` | minifilter — **enforces removable storage device rules** | yes |
| `hdlphook` | "Hook driver" (no further detail) | yes |
| `hdlpdbk` | device filter — **enforces plug-and-play device rules** | **no** |
| `hdlpnetmonitor` | **not documented anywhere on docs.trellix.com** | yes |

Three things follow.

**`hdlpflt` is the *removable storage* enforcement point, not a general file filter.** The freefixer
/ herdProtect style file-database pages describe it as "McAfee DLP Mini File Filter Driver", which is
just the driver's `VERSIONINFO` string; Trellix's own table is the better source and is more
specific. This matters because #57 named `hdlpflt` as the seat of "its own device control" — that
is half right. `hdlpflt` polices *file I/O on mounted volumes*. It is `hdlpdbk` that polices
*devices*.

**`hdlpdbk` is missing, and it is exactly the driver plug-and-play blocking runs on.** See §3.4.

**`hdlpnetmonitor` returns zero hits across the whole Trellix documentation portal.**
**[INFERENCE]** it is the driver behind Network Communication Protection rules, since that is the
only DLP Endpoint feature documented as having a dedicated driver that the table above omits —
"*Operational Mode and Modules* — Activate or deactivate the network communication driver (activated
by default)"
([Client configuration support for data protection rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d5a3bfca-19df-156d-ceec-8c7fa36f5dfc.html)).
Network communication protection governs *network shares and sockets*, not USB — but this is an
inference from an absence and should be treated as weak.

### 1.1 The `hdlphook` claim in #57 is not sourced

Issue #57 states, and #40's comment repeats, that "`hdlphook` is the clipboard / screen-capture
interception component". **No primary source was found that says this.** Trellix's driver table
calls it only "Hook driver". No product-guide, release-note, or interface-reference page found
describes its function.

**[INFERENCE]** The attribution is *plausible*: clipboard protection, screen-capture protection and
printer protection are the three DLP Endpoint features that must observe user-mode application
behaviour rather than file or device I/O, and "hook" is the conventional name for that mechanism.
But the project's own standard — the reason #40 exists — is that plausible-and-unsourced gets
marked, not asserted. Treat "`hdlphook` does the clipboard" as unverified until the Diagnostic Tool
or a KB confirms it.

---

## 2. Clipboard protection

### 2.1 What the feature is, in Trellix's words

> **Controlling copy-paste**
>
> Clipboard protection rules manage content copied with the Windows clipboard. They are supported on
> Trellix DLP Endpoint for both Windows and macOS.
>
> Clipboard protection rules are used to block or request justification for copying sensitive content
> from one application to another. The rule can define both the application copied from and the
> application copied to, or you can write a general rule specifying any application for either source
> or destination. Supported browsers can be specified as applications. The rule can be filtered with
> an end-user definition to limit it to specific users. As with other data protection rules,
> exceptions to the rule are defined on the **Exceptions** tab.
>
> By default, copying sensitive content from one Microsoft Office application to another is allowed.
> If you want to block copying within Microsoft Office, disable the Microsoft Office clipboard in the
> Windows client configuration and in the macOS client configuration.

— [Controlling copy-paste](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-958cec32-49db-b67a-5831-37c77ba377f4.html)

Note the framing: **"from one application to another"**. The rule model is a *pair* of applications,
not a clipboard-content ACL. §2.5 is where that becomes load-bearing.

### 2.2 The rule's condition fields

From the interface reference for the rule page:

> **Classification** — Use the `is any data (ALL)` option to bypass applying a content
> classification, or use the `is one of (OR)` or `is all of (AND)` options to select predefined
> classifications. […]
>
> **End-User** — Select a user group from the drop-down list. […]
>
> **Source application** — Select the paste-from application from the built-in list.
>
> **Destination application** — Select the paste-into application from the built-in list.

and, above the condition table:

> **Note:** All fields in this section are required. The default `ALL` can be used instead of a
> defined parameter.

— [Clipboard Protection rule page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-ab0d967b-8d1b-0457-6baa-73aa7253d3be.html)

So the discriminators are, in order of relevance to us: **content classification**, **source
application**, **destination application**, **user group** — each of which can be `ALL`. A rule of
the form *"any content, from any application, into any application → Block"* is expressible.

An "application" is an **Application Template** definition, whose available properties are:

> Command Line · Executable Directory · Executable file hash · Executable file name ·
> Original Executable file name · Product Name · Vendor Name · Window Title

— [Application Template definition page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-5c0215c4-ed42-5f51-db6c-17f63dfb4aa5.html)

Every one of those is available for an unsigned third-party binary. `Vendor Name` and `Product Name`
even have documented behaviour for binaries with no version resource: "If no name is listed, it
appears as `Unknown Company`" / `Unknown Product`. An administrator can therefore write a rule
targeting *unidentified* executables as a class, without knowing about our helper specifically.
**[INFERENCE]**, but a short one — that is what the field is for.

### 2.3 The crux: unclassified content is **not** structurally exempt

Issue #57 reasons that "Trellix DLP's clipboard protection rules act on *classified* content, which
is exactly the content worth relaying", and treats #40's pass on unlabeled content as therefore
non-generalising. The first half of that is wrong in an important way.

Trellix documents exactly one rule type where `ALL` classification and `Block` are mutually
exclusive, and it is not this one:

> **Available actions for data protection rules**
>
> | Data protection rule | Reactions | Additional information |
> | --- | --- | --- |
> | Application File Access Protection | Block | When the classification field is set to `is any data (ALL)`, the block action is not allowed. Attempting to save the rule with these conditions generates an error. |
> | **Clipboard Protection** | **Block** | *(no restriction stated)* |

— [Data protection rule actions](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-42a839d5-6351-b9ac-c74d-fee249dbe164.html)

**[INFERENCE]** A Clipboard Protection rule with `Classification = is any data (ALL)` and
`Action = Block` is a savable, deployable configuration. Trellix explicitly calls out the
console-side ban for one rule type and stays silent for this one, in a table whose whole purpose is
to enumerate per-rule restrictions. That is a strong reading of an absence, not a certainty — but it
inverts the assumption the project was about to build on.

Two consequences for the relay design:

- **A passing clipboard read of unlabeled content is not evidence about a differently-labeled
  payload, and a passing read of labeled content is not evidence about a different destination
  application.** The rule matrix has four independent axes; #40 sampled one cell.
- Conversely, the fact that clipboard reads work *at all* today is decent evidence that no
  `ALL/ALL/ALL → Block` rule is deployed, since such a rule would break ordinary copy-paste for
  everyone. **[INFERENCE]** The realistic deployed shape is a narrower rule keyed on classification
  or on a destination-application list.

### 2.4 What the actions are, and what a block looks like

> | Reaction | Applies to rules | Result |
> | --- | --- | --- |
> | No Action | All | Allows the action. |
> | Block | Data Protection, Device Control | **Blocks the action.** |
> | Request justification | Data Protection | Produces a pop-up on the end-user computer. The user selects a justification (with optional user input) or selects an optional action. |
> | Report Incident | All | Generates an incident entry of the violation in DLP Incident Manager. |
> | User notification | Data Protection, Device Control, Endpoint Discovery, Web Protection for Prevent | Sends a message to the endpoint to notify the user of the policy violation. **Note:** When User Notification is selected, and multiple events are triggered, the pop-up message states: *There are new DLP events in your DLP console*, rather than displaying multiple messages. |

— [Reactions available for rule types](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-09c28362-f3c1-07d9-0e43-6f788b091f88.html),
whose per-rule matrix marks Clipboard protection as supporting
`No action`, `Block`, `Report Incident`, `Request justification`, `Store original file as evidence`,
and `User notification`.

**[UNVERIFIED] — and this is the single most operationally important gap in this document.** Trellix
never describes what "Blocks the action" means at the Win32 API level. Specifically, nothing found
says whether a blocked clipboard operation surfaces to the calling process as:

- `OpenClipboard` / `GetClipboardData` failing,
- succeeding but returning empty or truncated data,
- succeeding but returning substituted content,
- or a modal pop-up that gates the operation synchronously (which `Request justification`
  self-evidently is, since the user must answer it, but its API-level effect on a *background*
  process with no visible window is undocumented).

The `Request justification` case is worth flagging separately for the design: a background helper
that triggers a justification prompt would put a modal dialog in front of a user who did not
initiate any copy-paste, attributed to an application they may not recognise. That is a
user-experience failure mode independent of whether the read succeeds.

The KB that most likely answers the API-level question — **KB85654, "Explanation of the clipboard
blocking feature"** — was unreachable (see the sourcing note). It should be read if anyone gets
access to a Trellix support login.

### 2.5 Client-side switches that gate the whole feature

Clipboard protection is not unconditionally on. Two client-configuration settings govern it:

> | Data protection rule | Client configuration page and settings |
> | --- | --- |
> | Clipboard Protection | **Operational Mode and Modules** — Activate the clipboard service. **Clipboard Protection** — Add or edit *ignored* processes. Enable or disable the Microsoft Office Clipboard. **Note:** Microsoft Office Clipboard is enabled by default. When enabled, you can't prevent copying from one Office application to another. |

— [Client configuration support for data protection rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d5a3bfca-19df-156d-ceec-8c7fa36f5dfc.html)

The ignored-processes list is a client-side allowlist, applied below the rules:

> You can specify ignored processes for clipboard and printer protection rules in the Policy Catalog
> Windows client configuration on their respective pages. Because these ignore list are applied at
> the client, they work with all clipboard, printer, and web protection rules. **Clipboard and printer
> protection rules ignore content produced by processes that are defined in the ignore list.**

— [Ignore lists](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-59039bbc-8e7b-ed41-69d7-1cd9e22c6091.html)

and the process is identified by original filename:

> **Process name** — Enter the original filename of the application in the text box to add it to
> *ignored processes*. **Note:** To find the original filename of an application, go to the source
> path of the application and right click on the .exe file, *Properties* → *Details* →
> *Original filename*.

— [Clipboard Protection page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-267b3dea-dc3a-8f93-e6cc-05af74a3a0c3.html)

Read that carefully: the ignore list exempts **content produced by** the listed process — i.e. it is
a *source*-side exemption, not a destination-side one. **[INFERENCE]** Adding the helper to the
ignore list would therefore not exempt the helper's *reads*; it would exempt content the helper
*writes*. That is the wrong direction for the eager-clipboard relay's inbound path being the risky
one, but it is arguably the right direction for the *outbound* path (content the helper places on
the clipboard after receiving it from the other machine would become invisible to clipboard rules).
Worth knowing if a conversation with the Trellix administrators ever happens; not something we can
do ourselves.

Note also the "original filename" mechanism: it reads the `OriginalFilename` field of the PE version
resource, which is a build-time attribute, not a signature. That is a per-build identity for an
unsigned binary — stable across rebuilds if the version resource is stable.

### 2.6 A structural question the documentation does not close

Everything above describes clipboard protection as governing *copy-paste between applications*. A
background helper calling `GetClipboardData` is not pasting into a document; it has no visible
window, initiates no `WM_PASTE`, and is driven by nothing the user did.

**[UNVERIFIED]** Whether the agent's interception point is (a) the clipboard API surface, so that any
`GetClipboardData` from any process is evaluated with that process as the "destination application",
or (b) a paste-gesture / window-message level hook, so that a programmatic read is simply not the
event the rule is written about. This is *the* question for the relay, and no primary source found
addresses it. §7 proposes the check.

Two weak indications, in opposite directions:

- The rule field is literally named "the paste-into application", suggesting a paste-shaped event.
- But the clipboard *service* is a separately activatable module with its own driver, and Trellix
  bills the feature as managing "content copied with the Windows clipboard", which is API-shaped
  language.

Neither is worth building on.

### 2.7 Version-specific wording

- The macOS availability line changed at 11.11: 11.10.x says "supported on Trellix DLP Endpoint for
  **Windows only**"
  ([11.10.x Controlling copy-paste](https://docs.trellix.com/bundle/data-loss-prevention-11.10.x-product-guide/page/UUID-eddad64b-47af-3b71-d066-b932f8e09a6f.html));
  11.11.x onward says "for both Windows and macOS". Confirmed by the
  [Trellix DLP Endpoint for Mac 11.11.0 Release Notes](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-mac-11.11.x-release-notes/page/UUID-66b6701d-0690-4448-76df-d060c05c85e9.html):
  "Clipboard Protection for macOS — Trellix DLP Endpoint for Mac now supports Clipboard Protection
  rules on macOS endpoints that allows you to monitor, block, or require justification for copying
  sensitive content between applications."
- The 11.14.x rule page still contradicts this, saying under `Enforce on`: "This rule type is only
  supported on Trellix DLP Endpoint for Windows." The interface-reference text lags the feature.
  Relevant only as a caution about how carefully to read these pages.
- Newer endpoint builds added a **Destination URL** condition not shown on the rule page: the
  [DLP Endpoint for Windows 11.11 Update 2 release notes](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.11.x-release-notes/page/UUID-59787323-546d-38eb-657e-0dc128e2d899.html)
  record "Fixed an issue where matched URL details were missing in incidents generated by the
  Clipboard Protection Rule when the and Destination URL option was set to Is any web URL." So the
  rule vocabulary is wider in the field than the product guide shows. Irrelevant to a serial helper,
  but a reason not to treat the documented field list as exhaustive.

---

## 3. Device control, and the USB CDC / composite question

### 3.1 The rule taxonomy

> Trellix DLP Endpoint for Windows supports the following types of rules:
> Citrix Virtual Apps and Desktops Device Rule · Fixed Hard Drive Rule · **Plug And Play Device Rule**
> · Removable Storage Device Rule · Removable Storage File Access Device Rule · TrueCrypt Device Rule

— [Device control rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-358fb878-3827-72e7-9302-c6238d0218ee.html)

Of these, only **Plug And Play Device Rule** could reach a CDC/composite device. The rest are
volume-, drive-letter-, or Citrix-scoped.

### 3.2 A CDC device is not "removable storage" — and this is documented, not inferred

The distinction is stated twice, in the same words, in two different chapters:

> - Plug-and-play device rules are triggered **when the hardware device is plugged into the
>   computer**. Since the reaction is to a **device driver**, the device class must be managed for
>   the device to be recognized.
> - Removable storage device rules are triggered **when a new file system is mounted**. When file
>   system mount occurs, the Trellix DLP Endpoint software associates the drive letter with the
>   specific hardware device and verifies the device properties. Since the reaction is to a **file
>   system operation**, not a device driver, the device class does not need to be managed.

— [Device control rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-dcc0f45e-6f25-aeb8-bc02-4b4e392e5c56.html)
and
[Benefits of device classes in managing devices](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d89c957a-72b2-0c03-ba90-40ed10a3d51b.html)

A USB CDC function loading `usbser.sys` produces a COM port. No volume is mounted, no drive letter is
assigned, no file system exists. **Removable-storage rules — and therefore `hdlpflt`, their documented
enforcement driver — cannot see it.** This is a documented conclusion, not an inference, and it
retires the "does Trellix think our device is removable storage?" question from #57 outright.

The removable-storage property list confirms it from the other side: the properties available on a
removable storage template are `File System Type` (CDFS/exFAT/FAT16/FAT32/NTFS/UDFS),
`File System Access`, `File System Volume Label`, `File System Volume Serial Number`, `CD/DVD
Drives`, `TrueCrypt devices`, plus USB VID/PID and serial —
[Device properties](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-5a24afd8-5a86-d8ba-aa43-d706fb841db7.html).
The defining properties are all file-system properties.

### 3.3 What a plug-and-play rule *can* match

The full property table, filtered to those usable in a Plug and Play template on Windows:

| Property | Description (quoted) |
| --- | --- |
| **Bus Type** | "Windows — Bluetooth, Firewire (IEEE1394), IDE/SATA, PCI, PCMIA, SCSI, **USB**, UAS (USB Attached SCSI) […] Selects the device BUS type from the available list." |
| **Device Class** | "Selects the device class from the **available managed list**." |
| **Device Compatible IDs** | "A list of physical device descriptions. Effective especially with device types other than USB and PCI, which are more easily identified using PCI VendorID/DeviceID or USB PID/VID." |
| **Device Instance Path** | "A Windows-generated string that uniquely identifies the device in the system. Example: `USB\VID_0930&PID_6533\5&26450FC&0&6`." |
| **Device Friendly Name** | "The name attached to a hardware device, representing its physical address." |
| **USB Class Code** | "Identifies a physical USB device by its general function. Select the class code from the available list." |
| **USB Device Serial Number** | "A unique alphanumeric string assigned by the USB device manufacturer […] The serial number is the last part of the instance ID." |
| **USB Vendor ID / Product ID** | "The USB VendorID and ProductID are embedded in the USB device […] Example: `USB\Vid_3538&Pid_0042`." |

— [Device properties](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-5a24afd8-5a86-d8ba-aa43-d706fb841db7.html)

Templates combine properties as "logical OR (by default) or logical AND. Multiple parameter types are
always added as logical AND", with the worked example "Bus Type is one of: FireWire (IEEE 1394) OR
USB AND Device Class is one of Memory Devices OR Windows Portable Devices" —
[Benefits of device templates](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-7e6854fe-56a9-aad4-d839-d2c6f2d4a4d3.html).

So **yes, a serial/COM device is in scope of the rule vocabulary.** `USB Class Code` is the exact
mechanism: the deskhopplus CDC function declares USB class `0x02` (Communications), which is what
makes `usbser.sys` bind at all —

> Windows loads the driver based on a compatible ID match […] `USB\Class_02` `USB\Class_02&SubClass_02`
> — To load *Usbser.sys* automatically, set the class code to 02 and subclass code to 02 in the
> Device Descriptor.

— [USB Serial Driver (Usbser.sys), Microsoft Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)

**[INFERENCE]** The same descriptor field that earns the driverless bind is the field a Trellix
plug-and-play rule would key on. A rule of the form *Bus Type = USB AND USB Class Code =
Communications → Block* is expressible in one template, and would hit the device without naming it.
Whether "Communications" is present in Trellix's "available list" of class codes is
**[UNVERIFIED]** — the list is not published; the page says only "Select the class code from the
available list."

Reactions are limited: the Device Control matrix gives Plug-and-play device rules only
**`No action`** and **`Block`** — no read-only, no justification —
[Reactions available for rule types](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-09c28362-f3c1-07d9-0e43-6f788b091f88.html).

### 3.4 The device class must be *managed*, and the enforcing driver is not loaded

Two independent preconditions gate plug-and-play blocking, and at least one of them is not met.

**Precondition 1 — a managed device class.**

> For plug-and-play device rules to control Microsoft Windows hardware devices, the device classes
> specified in device templates used by the rule **must be set to `Managed` status**.

— [Device control rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-dcc0f45e-6f25-aeb8-bc02-4b4e392e5c56.html)

Classes come in three statuses — `Managed`, `Unmanaged` ("devices Trellix Device Control doesn't
manage in the default configuration"), and `Excluded` — and the built-in list "can't be edited"
though it "can be duplicated and changed to add user-defined classes"
([Benefits of device classes](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d89c957a-72b2-0c03-ba90-40ed10a3d51b.html)).

**[UNVERIFIED] — Trellix does not publish which built-in classes are `Managed` by default.** Searching
the entire documentation portal for `"Ports (COM & LPT)"` returns zero results. The Windows classes
in play here are, from Microsoft's own table:

| Windows class | GUID |
| --- | --- |
| **Ports (COM & LPT ports)** — "Includes serial and parallel port devices." | `4d36e978-e325-11ce-bfc1-08002be10318` |
| **USB Device** — "USBDevice includes all USB devices that don't belong to another class." | `88BAE032-5A81-49f0-BC3D-A4FF138216D6` |
| **Human Interface Devices (HID)** | `745a17a0-74d3-11d0-b6fe-00a0c90f57da` |

— [System-Defined Device Setup Classes Available to Vendors, Microsoft Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/system-defined-device-setup-classes-available-to-vendors)

A `usbser.sys`-backed CDC interface lands in **Ports**; the composite parent is enumerated by
`usbccgp` under the USB system class; the HID interfaces land in **HIDClass**. Which of those the
local Trellix policy marks `Managed` is exactly the thing an unelevated check can read out of the
registry (§7).

**Precondition 2 — the device blocking module, and its driver.** The client configuration has an
explicit switch:

> **Data Protection Modules → Device Blocking** — When selected, activates device rules that are
> configured in the policy.

— [Operational Mode and Modules page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-ee0e90a4-12ef-5c18-a7e1-57f9bf19760e.html)

and the driver that implements it is **`hdlpdbk` — "Trellix DLP Endpoint device filter driver
(enforces device Plug and Play rules)", "can be disabled in configuration"**
([Checking the agent status](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-e196c74d-9149-c083-3f4d-2028b184ab44.html)).

**`hdlpdbk` is not among the drivers found running on the machine in #40.** The five that were found
— `hdlpctrl`, `hdlpevnt`, `hdlpflt`, `hdlphook`, `hdlpnetmonitor` — account for control, events,
removable-storage file filtering, the hook, and (probably) network monitoring. The device-blocking
filter driver is the one absent.

**[INFERENCE], and the most consequential one in this document:** plug-and-play device control is
switched off in this deployment. Trellix names exactly one driver as the enforcement point for
plug-and-play rules, documents that it is the one driver in the list that "can be disabled in
configuration", provides an explicit `Device Blocking` module toggle that would do the disabling,
and the driver is not loaded. The reasoning is sound but it is still an inference from a driver
inventory, and it should be retired by measurement (§7) rather than trusted — precisely the pattern
#40 established. It is also *revocable at any time by a policy push*, which is a different kind of
fragility from a design constraint.

Note additionally, for the config-mode round trip in standing decision 1 of map #31:

> Device control rules trigger **ONLY when a device is plugged in**. The notification sent (block,
> read-only, report incident) is based on the rule action. User actions on a plugged-in device don't
> cause more incidents to be logged or notifications to be sent.

— [Device control rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-dcc0f45e-6f25-aeb8-bc02-4b4e392e5c56.html)

and

> **Device Control Settings (Plug and Play)** — Enables or disables enforcing the policy immediately.
> When disabled, policies are only enforced when the Trellix DLP Endpoint client is restarted, or
> **when the device is physically or logically enabled/disabled**. Default: Enabled.

— [Device control page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-42e6c5f1-852c-c456-2c1c-84200def2d3d.html)

**[INFERENCE]** Because the device re-enumerates under a different USB identity on every config-mode
round trip, each round trip is a fresh device-plug event and a fresh rule evaluation. If a rule ever
does apply, it applies per re-enumeration, and a policy pushed while the device is attached takes
effect at the next re-plug rather than immediately.

### 3.5 This is not the same mechanism as Windows Device Installation Restrictions

Worth stating plainly, because the two get conflated and #36's analysis was built on the Microsoft
one.

| | Windows `DeviceInstall\Restrictions` | Trellix plug-and-play device rule |
| --- | --- | --- |
| Acts on | **Driver installation** — whether a driver may be installed for a device ID/class | **The device stack at plug-in** — a class filter driver |
| Configured via | GPO / MDM, visible in `HKLM\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions` | ePO policy, **invisible** in those keys |
| State on this machine | **every key absent** (#40) | `hdlpdbk` not loaded (§3.4) |
| Blocks `USB\Composite`? | Yes — Microsoft's own worked example does exactly this | Only if a template matches and the class is managed |

The Device Class definition page's `Filter Type` field — "Upper or lower. Most devices use the upper
filter"
([Device Class definition page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-9a26af47-ac0c-2b12-ec58-0c1bd789396a.html))
— is the direct confirmation that the mechanism is a **class filter driver**, i.e. `UpperFilters` /
`LowerFilters` on the device setup class key.

**[INFERENCE]** Therefore Trellix cannot prevent `usbser.sys` from *installing*; it can only cause the
resulting device to fail or be disabled once the stack is built. The observable symptom would be a
device present in Device Manager with an error state, not an absent driver — which is a different
diagnostic signature from the Windows-policy case, and worth knowing when the check finally runs.

**Corollary for the degrade-to-HID fallback.** The `windows-helper-constraints.md` finding that a
`USB\Composite` block "takes the HID interfaces with it" was reasoned about Windows device-install
policy. Under a Trellix plug-and-play rule the blast radius depends on which device class the
template names: a rule keyed on the **Ports** class or on `USB Class Code = Communications` would
**[INFERENCE]** hit only the CDC function and leave the HID interfaces working, whereas one keyed on
the composite parent or on VID/PID would take the whole device. So the fallback is not uniformly
doomed here — it depends on the rule's shape, which we cannot see.

---

## 4. Observability without administrator rights

### 4.1 The end-user console — available, if the admin left it on

> **Endpoint console** — The endpoint console was designed to share information with the user and to
> facilitate self-remediation of problems. It is configured on the *Windows Client Configuration* →
> *User Interface Service* tab.
>
> The console is activated from the icon in the notification area by selecting *Manage Features* →
> *DLP Endpoint Console*. It can also be activated by double-clicking **`DlpConsoleRunner.exe`** in the
> **`C:\Program Files\McAfee\DLP\Agent\Tools\`** folder. Fully configured, it has four tabbed pages:
>
> - **Notifications History** — Displays events, including details of aggregated events.
> - **Discovery** — Displays details of discovery scans.
> - **Tasks** — Generates ID codes and enter release codes for agent bypass and quarantine.
> - **About** — Displays information about **agent status, active policy, configuration, and computer
>   assignment group, including revision ID numbers**.

— [Benefits of protecting Windows endpoints](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-10596399-4eeb-c0f0-6279-fe50f99387ed.html)

This is the answer to "did Trellix block it, or is it our bug": **Notifications History**. No
elevation is documented as required, and the tool sits in a world-readable Program Files path. The
caveats: the console's availability is a policy setting ("Fully configured, it has four tabbed
pages" — implying partial configurations exist), and the *About* tab gives policy **revision IDs**,
not rule text. It will not tell us *which rule* fired, only that something did.

### 4.2 The diagnostic tool — not available to a standard user

`hdlpDiag.exe`, in the same `Tools` folder, is far richer — it has an **Active policy** tab that
"Displays all rules contained in the active policy, and the relevant policy definitions", and a
**Devices** tab that "Displays all Plug and Play and removable devices currently connected to the
computer […] Displays all active device control rules and relevant definitions from the device
definitions"
([Diagnostic Tool](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-a150dfd3-a771-9210-7017-749a6bc4e736.html)).

That would answer every open question in this document at once. It is gated:

> Diagnostic Tool requires authentication with DLP Help Desk. […] Copy the Identification Code to the
> *Help Desk* → *Identification Code* text box on the *Generate DLP Client Bypass Key* page. Fill in
> the rest of the information and generate a Release Code. Copy the Release Code to the
> authentication window *Validation Code* text box and click OK.

— [Run the Diagnostic Tool](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-77c07b38-92e6-da2f-9275-9bd7f0f7d0f8.html)

The release code is generated in the ePO console by someone with the DLP Help Desk permission set.
**A standard user cannot run it.** If a relationship with the endpoint team is available, asking them
to run `hdlpDiag.exe` once and read out the *Active policy* and *Devices* tabs is by a wide margin
the cheapest way to close this whole file.

### 4.3 Where enforcement events go

Events go **off the box**, to ePO:

> Events generated by the Trellix DLP Endpoint client software are sent to the ePO - On-prem Event
> Parser, and recorded in tables in the ePO - On-prem database.

— [How DLP Endpoint and Device Control protect sensitive content](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-fe9fff04-cfa5-c4cb-e65b-5c42d3c5de80.html)

Optional additional sinks exist, both admin-configured and both remote: "Log DLP events to external
HTTP server" and a syslog server on UDP/514
([Debugging and Logging page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-851cc941-e345-33f0-ffd4-524ea96d48cf.html)).
That page also documents the client's own logging — "Save log messages to files", "Print log messages
to DebugView (Win) and Console (OSX)", with levels "Log all messages (for debugging) / Warnings and
Errors / Errors only" — but **[UNVERIFIED]** where those files are written. No Trellix page found
states a client log path, and a portal search for `ProgramData\McAfee\DLP` returns zero results.

Note the DebugView option: if the admin has enabled "Print log messages to DebugView", DLP log output
goes to the Win32 `OutputDebugString` channel, which an unelevated process **can** read. That is a
long shot (it is a troubleshooting setting, unlikely to be on in production) but it is free to test.

**[UNVERIFIED]** Whether the Windows Event Log carries any DLP channel. Nothing found says it does.

**CORRECTION (measured 2026-08-09, Part G).** The client-artefact paths above are no longer
unverified — documentation not stating a path did not mean no path exists. All of the following are
readable by the standard user, without elevation:

| Path | Size | Last write | What it looks like |
| --- | --- | --- | --- |
| `C:\ProgramData\McAfee\DLP\Agent\Policy\SCM.opg` | 155 KB | 2026-08-07 | the policy cache |
| `C:\ProgramData\McAfee\DLP\Temp\Policy\Configuration.opg` | 9.5 KB | 2026-05-29 | client configuration |
| `C:\ProgramData\McAfee\DLP\Agent\S-1-5-…\NotificationList.opg` | 1.5 KB | 2026-08-08 | notification history backing the console |
| `C:\ProgramData\McAfee\DLP\Temp\TextExtractorDump…log` | 4.8 KB | 2026-03-04 | text-extractor debug output |
| `C:\ProgramData\McAfee\DLP\Agent\ChromiumExtension\DlpExtension.crx` | 35 KB | 2025-05-28 | browser-side DLP extension |

The event-log finding stands: `Get-WinEvent -ListLog *` filtered for `DLP|McAfee|Trellix` returns no
non-empty channel readable by a standard user. Enforcement telemetry does go off the box to ePO as
documented — but the **policy** the agent enforces is cached locally and is world-readable, which the
documentation nowhere says.

**Answered (Part H, measured 2026-08-09): `.opg` is encrypted. The local route is closed.**

| File | Entropy | Verdict |
| --- | --- | --- |
| `SCM.opg` | **7.98** bits/byte | encrypted |
| `Configuration.opg` | **7.95** | encrypted |
| `NotificationList.opg` | **7.55** | encrypted (small sample) |
| `TextExtractorDump*.log` | 3.55 | plain UTF-16 text |

All three `.opg` files share a 16-byte header — magic `4F 50 47 A1` (`OPG\xA1`), a version field, then
a little-endian length at offset 12 that equals *filesize − 16* exactly for `Configuration.opg` and
`NotificationList.opg`. The payload is a single high-entropy blob rather than a container of
individually readable records, so no partial recovery is available. 8,224 printable runs were
extracted from `SCM.opg` and every one was noise; zero policy keywords matched across all three files.

**Conclusion: the policy the agent enforces cannot be read locally by any unelevated means found
here.** The remaining routes to knowing which clipboard and device rules are in force are
`hdlpDiag.exe` via the endpoint team (needs a Help-Desk release code, §4.2), or empirical probing of
one behaviour at a time.

`TextExtractorDump*.log` *is* readable — plain UTF-16 — and shows `Fcagte`'s per-request parameters
including full source file paths, with `CalcFileHash = 1`. It confirms content inspection is active on
the file path, but the sampled dump concerns a browser download and contains nothing about clipboard
operations. It also appears to be written only on a slow-extraction event ("Text Extractor Worse
Execution Time: 142 Seconds") rather than routinely, so it is not a general-purpose observation
channel.

Note also `TextExtractorDump*.log`: the `Fcagte` text-extractor process writes debug output to disk
here, which is the component that would see clipboard and file content on its way through
classification.

---

## 5. Version and product naming — how much to trust each claim

**The lineage.** McAfee DLP Endpoint → Trellix DLP Endpoint. The rename is documented in the
[DLP Endpoint for Windows 11.6 Update 7 release notes](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.6.700-higher-release-notes/page/GUID-B6C9B3EC-C816-4E5F-AAB7-2B3101130DB5.html):
"Product name - McAfee Data Loss Prevention Endpoint is renamed as Trellix DLP Endpoint."

**The service name pins a version floor.** The
[11.6 Update 8 (11.6.800) release notes](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.6.700-higher-release-notes/page/GUID-8A3A37A6-38BF-419E-B707-3B623716B384.html)
(release date 3 August 2023, client build 11.6.800.22) state: "You will notice **McAfeeDLPAgentService
is renamed as TrellixDLPAgentService**." The machine reports `TrellixDLPAgentService`, so it runs
**≥ 11.6.800** (or ≥ 11.10.100, which carries the same note). The exact build is unknown and worth
capturing (§7), because the clipboard feature set moved between 11.10 and 11.11.

**Paths did not rebrand.** The install tree is still `C:\Program Files\McAfee\DLP\Agent\`, and the
drivers are still `hdlp*` — "hdlp" for *Host DLP*, the pre-11.x product name. Documentation for
McAfee DLP Endpoint 11.1.x/11.3.x therefore remains substantively applicable: the driver table, the
device-class model, the clipboard rule fields and the reactions matrix are **identical strings**
across 11.1.x → 11.14.x. Where this document cites 11.14.x, the same text was verified present in
11.1.x/11.3.x unless a version difference is called out (§2.7 is the only one found).

**How much to trust each claim, by source type:**

| Source | Confidence |
| --- | --- |
| Product guide / interface reference (rule fields, reactions, driver table, device properties) | High — first-party, stable across nine major versions |
| Release notes (version floors, feature arrival dates) | High, but narrow |
| Absence of a documented restriction read as permission (§2.3) | Medium — marked [INFERENCE] |
| Driver-role attributions beyond Trellix's own one-line table (`hdlphook` = clipboard, `hdlpnetmonitor` = network) | Low — marked [INFERENCE]/[UNVERIFIED] |
| Third-party file-database sites (freefixer, herdProtect, systemexplorer) | Not used as the basis of any claim; they only echo the drivers' embedded `VERSIONINFO` strings |

---

## 6. What this changes in `windows-helper-constraints.md`

Recorded here rather than editing that file, per the task's single-file scope. Issue #57 item 1 asks
for the edits; these are the specific ones the evidence supports.

1. **§ on Purview/Endpoint DLP** — keep the reasoning trail, but mark it *not applicable to the
   target machine*: Defender and Purview are dormant there (#40). It stays relevant to any other
   target machine.
2. **The device-installation-policy section** — its conclusion that "device-installation policy can
   block the composite device retroactively" is *correct as Windows mechanism* and *unconfigured in
   fact* (#40: every `DeviceInstall\Restrictions` key absent). The live successor risk is a Trellix
   plug-and-play rule, which is a different mechanism (§3.5), currently apparently inert (§3.4), and
   whose blast radius on the HID interfaces depends on the rule's shape rather than being uniformly
   fatal.
3. **The clipboard section** — the Windows-level conclusion (clipboard is a shared USER resource, not
   UIPI-isolated) survives intact and was confirmed empirically by #40's Part E. Trellix sits *above*
   that as an independent enforcement layer with its own, differently-shaped rules; the two findings
   do not conflict and neither substitutes for the other.
4. **Add** the §2.3 correction: unlabeled content is not structurally exempt from clipboard rules.

---

## 7. What documentation could not settle — checks to run on the machine

The pattern from #40: retire inferences with measurements. Everything below is cheap, unelevated,
and read-only unless marked otherwise. Ordered by value.

### Group A — device control (settles §3.4, the biggest inference here)

**A1. Confirm `hdlpdbk` is genuinely absent, not merely stopped.**
```powershell
Get-CimInstance Win32_SystemDriver | Where-Object Name -like 'hdlp*' |
  Select-Object Name, DisplayName, State, StartMode, PathName
Get-ChildItem 'C:\Windows\System32\drivers\hdlp*.sys' |
  Select-Object Name, Length, @{n='Ver';e={$_.VersionInfo.FileVersion}}
```
Distinguish three cases: *service not registered at all* (module never installed), *registered but
`Stopped`/`StartMode=Disabled`* (module disabled by policy — revocable at any time), *running*
(the whole §3.4 inference is wrong). The file's presence on disk with no registered service is the
strongest form of "installed but switched off".

**A2. Read the device-class filter registry directly — this is the ground truth for §3.4/§3.5.**
```powershell
'{4d36e978-e325-11ce-bfc1-08002be10318}',  # Ports (COM & LPT)
'{88BAE032-5A81-49f0-BC3D-A4FF138216D6}',  # USBDevice
'{745a17a0-74d3-11d0-b6fe-00a0c90f57da}',  # HIDClass
'{36fc9e60-c465-11cf-8056-444553540000}' | ForEach-Object {   # USB (system)
  $k = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$_"
  [pscustomobject]@{
    Class        = (Get-ItemProperty $k -EA SilentlyContinue).Class
    UpperFilters = (Get-ItemProperty $k -Name UpperFilters -EA SilentlyContinue).UpperFilters
    LowerFilters = (Get-ItemProperty $k -Name LowerFilters -EA SilentlyContinue).LowerFilters
  }
}
```
`HKLM\SYSTEM\CurrentControlSet\Control\Class` is readable by standard users. If `hdlpdbk` appears in
no `UpperFilters`/`LowerFilters` list for the Ports, USBDevice, USB or HIDClass keys, Trellix has no
filter on the path our device would take — which is a *stronger and more direct* result than the
driver inventory, because it answers "managed device class" and "device blocking active" in one
read. This is the single highest-value check in this document.

**A3. Record the agent build, so the documentation above can be version-matched.**
```powershell
Get-ChildItem 'C:\Program Files\McAfee\DLP\Agent' -Recurse -Filter '*.exe' -EA SilentlyContinue |
  Select-Object Name, @{n='Ver';e={$_.VersionInfo.FileVersion}},
                      @{n='OrigName';e={$_.VersionInfo.OriginalFilename}}
```
Confirms ≥ 11.6.800 and pins which product-guide version applies (§2.7 matters between 11.10 and
11.11).

**A4. When the CDC interface exists, check its device state and stack.**
```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_*' |
  Select-Object Class, FriendlyName, Status, Problem, InstanceId
Get-PnpDeviceProperty -InstanceId '<our device>' -KeyName `
  'DEVPKEY_Device_Service','DEVPKEY_Device_UpperFilters','DEVPKEY_Device_LowerFilters',
  'DEVPKEY_Device_ClassGuid','DEVPKEY_Device_CompatibleIds'
```
A Trellix block should present as a device that is *present with an error/problem code* rather than a
device with no driver (§3.5) — that signature distinguishes "Trellix blocked it" from "the descriptor
is wrong and `usbser.sys` never matched", which is otherwise easy to confuse.

**A5. Free control:** existing COM ports already bind fine on this machine (#40: Bluetooth SPP ×4,
Intel AMT SOL, all `status=OK`). Re-record their `Status` alongside A4 so a failure of *our* device is
visibly specific to it rather than to the Ports class.

### Group B — clipboard (settles §2.3 and §2.6, the highest-risk unknowns)

**B1. The labeled-content read.** Repeat #40's check 3, but copy from a document that carries a real
organisational classification — the strongest available label, not a test string. Record, per read:
whether `GetClipboardData` succeeded; the byte count; the **full format list** (`EnumClipboardFormats`
plus `GetClipboardFormatName`, so any Trellix- or MSIP-specific private format is captured by name);
and whether any Trellix pop-up appeared. This is the measurement #57 asks for.

**B2. Separate "read" from "paste" — the §2.6 question.** Same clipboard content, two consumers, back
to back:
- (a) the helper-equivalent process calling `OpenClipboard`/`GetClipboardData` with no window shown;
- (b) a real paste (`Ctrl+V`) into Notepad, and into a second app.

If (b) is blocked and (a) is not, enforcement is paste-gesture-shaped and the relay's read path is
outside it. If both are blocked, it is API-shaped. If neither, no clipboard rule matches this content
and the test needs stronger content before it means anything. **Record the negative case explicitly**
— "nothing was blocked" is only informative alongside evidence that a blocking rule exists at all,
which B4 supplies.

**B3. Characterise the failure mode** (only if B1 or B2 blocks). Capture the exact Win32 result:
`OpenClipboard` return + `GetLastError`; `GetClipboardData` return; on success, the handle's size and
first bytes. This is the **[UNVERIFIED]** in §2.4 and determines whether the helper can even *detect*
a block, or whether it silently relays empty/substituted content — the difference between a graceful
degradation and a correctness bug. Note #40's warning that `GetLastError` reads stale (203) unless
cleared with `SetLastError(0)` immediately before the call.

**B4. Read the notification history.** Launch `C:\Program Files\McAfee\DLP\Agent\Tools\DlpConsoleRunner.exe`
unelevated after B1–B3. Record: whether it launches at all; whether *Notifications History* has
entries corresponding to the tests; what *About* reports for agent status, operation mode, active
policy and revision IDs. This both answers §4.1's "is the console enabled here" and gives the
"Trellix blocked it vs our bug" discriminator the relay's logging will need.

**B5. Cheap long shot — the DebugView channel.** Attach a plain `OutputDebugString` reader (or
`Get-WinEvent -ListLog *` filtered for provider names containing `DLP`/`McAfee`/`Trellix`) while
running B1–B2. Costs a minute; if the admin left client logging on the DebugView sink (§4.3), it
hands us a running commentary from the agent itself.

**B6. Locate the client's own artefacts**, since no documentation gives the path:
```powershell
Get-ChildItem 'C:\ProgramData\McAfee\DLP','C:\ProgramData\Trellix' -Recurse -Depth 3 -EA SilentlyContinue |
  Select-Object FullName, Length, LastWriteTime
```
Read-only enumeration; note which paths a standard user can actually open. Resolves the
**[UNVERIFIED]** in §4.3.

### Group C — the question no script can answer

**C1. Ask the endpoint team to run `hdlpDiag.exe` once** and read out the **Active policy** and
**Devices** tabs (§4.2). One conversation closes §2.3, §2.6, §3.3 and §3.4 simultaneously, with
authoritative answers rather than inferences. Everything in Group A and B is a proxy for this.

---

## Sources

**Trellix** (all under `docs.trellix.com`; content read via the portal's prerender host
`docs-be.trellix.com` at identical paths):

- [Controlling copy-paste](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-958cec32-49db-b67a-5831-37c77ba377f4.html) (11.14.x) and [11.10.x](https://docs.trellix.com/bundle/data-loss-prevention-11.10.x-product-guide/page/UUID-eddad64b-47af-3b71-d066-b932f8e09a6f.html)
- [Clipboard Protection rule page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-ab0d967b-8d1b-0457-6baa-73aa7253d3be.html)
- [Clipboard Protection page (client configuration)](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-267b3dea-dc3a-8f93-e6cc-05af74a3a0c3.html)
- [Client configuration support for data protection rules](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d5a3bfca-19df-156d-ceec-8c7fa36f5dfc.html)
- [Data protection rule actions](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-42a839d5-6351-b9ac-c74d-fee249dbe164.html)
- [Reactions available for rule types](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-09c28362-f3c1-07d9-0e43-6f788b091f88.html)
- [Ignore lists](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-59039bbc-8e7b-ed41-69d7-1cd9e22c6091.html)
- [Application Template definition page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-5c0215c4-ed42-5f51-db6c-17f63dfb4aa5.html)
- [How applications are categorized](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-c270052b-e4a2-da99-7e5a-f55760183983.html)
- [Operational Mode and Modules page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-ee0e90a4-12ef-5c18-a7e1-57f9bf19760e.html)
- [Device control rules (concept)](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-358fb878-3827-72e7-9302-c6238d0218ee.html) and [Device control rules (detail)](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-dcc0f45e-6f25-aeb8-bc02-4b4e392e5c56.html)
- [Plug And Play Device Rule page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-efa6694c-279a-c9ba-f512-72cf48d93b0c.html)
- [Device properties](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-5a24afd8-5a86-d8ba-aa43-d706fb841db7.html)
- [Benefits of device classes in managing devices](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-d89c957a-72b2-0c03-ba90-40ed10a3d51b.html)
- [Benefits of device templates in defining device parameters](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-7e6854fe-56a9-aad4-d839-d2c6f2d4a4d3.html)
- [Create a device template](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-f1c8d1e6-4743-66a6-1f7e-527471f8b315.html) · [Create an excluded plug-and-play template](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-3e5c0077-c5a8-0216-b8b5-d4ead127d843.html)
- [Device Class definition page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-9a26af47-ac0c-2b12-ec58-0c1bd789396a.html) · [Device Templates page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-0b24646f-488e-1611-aacd-1e9f94b9a9b2.html)
- [Device control page (client configuration)](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-42e6c5f1-852c-c456-2c1c-84200def2d3d.html)
- [How DLP Endpoint and Device Control protect sensitive content](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-fe9fff04-cfa5-c4cb-e65b-5c42d3c5de80.html)
- [Benefits of protecting Windows endpoints (endpoint console)](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-10596399-4eeb-c0f0-6279-fe50f99387ed.html)
- [Diagnostic Tool](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-a150dfd3-a771-9210-7017-749a6bc4e736.html) · [Run the Diagnostic Tool](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-77c07b38-92e6-da2f-9275-9bd7f0f7d0f8.html) · [Checking the agent status](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-e196c74d-9149-c083-3f4d-2028b184ab44.html)
- [Debugging and Logging page](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-851cc941-e345-33f0-ffd4-524ea96d48cf.html)
- [Customizing end-user messages](https://docs.trellix.com/bundle/data-loss-prevention-11.14.x-product-guide/page/UUID-53015b21-6a15-1022-fed6-d714e9a28656.html)
- Release notes: [DLPE for Windows 11.6 Update 7](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.6.700-higher-release-notes/page/GUID-B6C9B3EC-C816-4E5F-AAB7-2B3101130DB5.html) · [11.6 Update 8](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.6.700-higher-release-notes/page/GUID-8A3A37A6-38BF-419E-B707-3B623716B384.html) · [11.11 Update 2](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-windows-11.11.x-release-notes/page/UUID-59787323-546d-38eb-657e-0dc128e2d899.html) · [DLP Endpoint for Mac 11.11.0](https://docs.trellix.com/bundle/data-loss-prevention-endpoint-mac-11.11.x-release-notes/page/UUID-66b6701d-0690-4448-76df-d060c05c85e9.html)

**Unreachable, and therefore not used:** `kcm.trellix.com` KB85654 ("Explanation of the clipboard
blocking feature") and KB89301 ("Trellix Data Loss Prevention 11.x.x Known Issues") — connection
refused throughout this research.

**Microsoft Learn:**

- [USB Serial Driver (Usbser.sys)](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)
- [System-Defined Device Setup Classes Available to Vendors](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/system-defined-device-setup-classes-available-to-vendors)
