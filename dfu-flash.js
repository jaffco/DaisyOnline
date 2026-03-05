/**
 * DaisyOnline/dfu-flash.js
 *
 * Standalone QSPI DFU flash helper for DaisyOnline.
 * Exposes window.dfuFlasher with:
 *
 *   dfuFlasher.flashFirmware(buffer, callbacks)
 *     Write the pre-compiled WASM-loader SRAM firmware to QSPI at 0x90040000.
 *     The Daisy bootloader copies the BOOT_SRAM binary to SRAM on every reset.
 *
 *   dfuFlasher.flashData(buffer, callbacks)
 *     Write user AOT binary (with 8-byte header) to QSPI at 0x90080000.
 *
 * Both functions open their own DFU session so that firmware and data flashes
 * can be performed independently without a combined USB session.
 *
 * callbacks: { onPhase(label), onProgress(done, total), onDone(), onError(err) }
 *
 * Requires dfu/dfu.js and dfu/dfuse.js to be loaded first.
 *
 * Memory layout (written by DaisyOnline):
 *   0x90000000  QSPI base — Daisy bootloader reservation
 *   0x90040000  BOOT_SRAM firmware  (written by flashFirmware)
 *   0x90080000  User AOT binary     (written by flashData)
 */
(function () {
    'use strict';

    const QSPI_FIRMWARE_ADDRESS = 0x90040000;
    const QSPI_DATA_ADDRESS     = 0x90080000;

    // ── Helpers ───────────────────────────────────────────────────────────────

    function hex4(n) {
        let s = n.toString(16);
        while (s.length < 4) s = '0' + s;
        return s;
    }

    function logMsg(msg)  { console.log('[DFU]', msg); }
    function logErr(msg)  { console.error('[DFU]', msg); }

    function makeProgress(nDone, nTotal) {
        logMsg('Progress: ' + nDone + ' / ' + nTotal + ' bytes');
    }

    // Read DFU functional descriptor to get transfer size and tolerant flag.
    async function getDFUDescriptorProps(device) {
        try {
            const data   = await device.readConfigurationDescriptor(0);
            const cfg    = dfu.parseConfigurationDescriptor(data);
            const cfgVal = device.settings.configuration.configurationValue;
            if (cfg.bConfigurationValue === cfgVal) {
                for (const desc of cfg.descriptors) {
                    if (desc.bDescriptorType === 0x21 && desc.hasOwnProperty('bcdDFUVersion')) {
                        return {
                            WillDetach:            !!(desc.bmAttributes & 0x08),
                            ManifestationTolerant: !!(desc.bmAttributes & 0x04),
                            CanUpload:             !!(desc.bmAttributes & 0x02),
                            CanDnload:             !!(desc.bmAttributes & 0x01),
                            TransferSize:          desc.wTransferSize,
                            DFUVersion:            desc.bcdDFUVersion,
                        };
                    }
                }
            }
        } catch (_) {}
        return {};
    }

    // Ensure interface names are populated (some devices don't fill them automatically).
    async function fixInterfaceNames(usbDevice, interfaces) {
        if (!interfaces.some(i => i.name == null)) return;
        let temp = new dfu.Device(usbDevice, interfaces[0]);
        await temp.device_.open();
        await temp.device_.selectConfiguration(1);
        const mapping = await temp.readInterfaceNames();
        await temp.close();
        for (const intf of interfaces) {
            if (intf.name === null) {
                const ci  = intf.configuration.configurationValue;
                const n   = intf['interface'].interfaceNumber;
                const alt = intf.alternate.alternateSetting;
                intf.name = mapping[ci][n][alt];
            }
        }
    }

    // ── Shared session helper ─────────────────────────────────────────────────

    /**
     * Show the browser USB device picker, find the QSPI DFUSe alternate,
     * open it and return a configured dfuse.Device ready for do_download.
     */
    async function _openQspiDevice(progressFn) {
        const usbDevice = await navigator.usb.requestDevice({ filters: [] });

        const allInterfaces = dfu.findDeviceDfuInterfaces(usbDevice);
        if (allInterfaces.length === 0) {
            throw new Error('No DFU interfaces found. Is the Daisy in bootloader mode?');
        }

        await fixInterfaceNames(usbDevice, allInterfaces);
        logMsg('Available DFU alternates: ' + allInterfaces.map(i => i.name).join(', '));

        // Prefer the alternate that covers the QSPI base address.
        const qspiIntf = allInterfaces.find(i => i.name && i.name.includes('0x90000000'))
                      || allInterfaces.find(i => i.name && /qspi/i.test(i.name))
                      || allInterfaces[0];

        if (!qspiIntf) {
            throw new Error('No QSPI DFU interface found. Is the Daisy in bootloader mode?');
        }
        logMsg('Using alternate: ' + qspiIntf.name);

        let dev = new dfu.Device(usbDevice, qspiIntf);
        await dev.open();

        const desc = await getDFUDescriptorProps(dev);

        let xferSize        = 1024;
        let manifestTolerant = true;
        if (desc && Object.keys(desc).length > 0) {
            if (desc.TransferSize)  xferSize         = desc.TransferSize;
            if (desc.CanDnload)     manifestTolerant = desc.ManifestationTolerant;
            if (desc.DFUVersion === 0x011a &&
                dev.settings.alternate.interfaceProtocol === 0x02) {
                dev = new dfuse.Device(dev.device_, dev.settings);
            }
        }

        dev.logDebug    = logMsg;
        dev.logInfo     = logMsg;
        dev.logWarning  = logMsg;
        dev.logError    = logErr;
        dev.logProgress = progressFn || makeProgress;

        dev._xferSize        = xferSize;
        dev._manifestTolerant = manifestTolerant;

        // Clear any stale error state
        try {
            const st = await dev.getStatus();
            if (st.state === dfu.dfuERROR) await dev.clearStatus();
        } catch (_) {}

        return dev;
    }

    // ── Public API ────────────────────────────────────────────────────────────

    /**
     * Flash the pre-compiled WASM-loader firmware to QSPI at 0x90040000.
     *
     * @param {ArrayBuffer} firmwareBuffer  Binary content of firmware/loader.bin
     * @param {object}      callbacks       { onPhase, onProgress, onDone, onError }
     */
    async function flashFirmware(firmwareBuffer, callbacks) {
        const cb = callbacks || {};
        const phase = l => { if (cb.onPhase) cb.onPhase(l); };
        const progress = (d, t) => { makeProgress(d, t); if (cb.onProgress) cb.onProgress(d, t); };

        try {
            phase('Connecting');
            const dev = await _openQspiDevice(progress);
            dev.startAddress = QSPI_FIRMWARE_ADDRESS;
            phase('Writing firmware');
            await dev.do_download(dev._xferSize, firmwareBuffer, dev._manifestTolerant);
            phase(null);
            if (cb.onDone) cb.onDone();
        } catch (err) {
            phase(null);
            logErr(err);
            if (cb.onError) cb.onError(err);
        }
    }

    /**
     * Flash the user AOT binary (with 8-byte header) to QSPI at 0x90080000.
     *
     * @param {ArrayBuffer} dataBuffer   AOT binary prefixed with 8-byte header
     * @param {object}      callbacks    { onPhase, onProgress, onDone, onError }
     */
    async function flashData(dataBuffer, callbacks) {
        const cb = callbacks || {};
        const phase = l => { if (cb.onPhase) cb.onPhase(l); };
        const progress = (d, t) => { makeProgress(d, t); if (cb.onProgress) cb.onProgress(d, t); };

        try {
            phase('Connecting');
            const dev = await _openQspiDevice(progress);
            dev.startAddress = QSPI_DATA_ADDRESS;
            phase('Writing AOT module');
            await dev.do_download(dev._xferSize, dataBuffer, dev._manifestTolerant);
            phase(null);
            if (cb.onDone) cb.onDone();
        } catch (err) {
            phase(null);
            logErr(err);
            if (cb.onError) cb.onError(err);
        }
    }

    // ── Exports ───────────────────────────────────────────────────────────────
    window.dfuFlasher = {
        flashFirmware,
        flashData,
        QSPI_FIRMWARE_ADDRESS,
        QSPI_DATA_ADDRESS,
    };

})();
