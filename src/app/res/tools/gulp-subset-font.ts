import asyncTransform from "./async-transform";
import sf from 'subset-font';
import {BufferFile} from "vinyl";

/**
 * harfbuzz keeps only name IDs 0-6 by default. 10 (description), 13 (licence)
 * and 14 (licence URL) must survive as well: they are how a font under the SIL
 * OFL -- fa-brands-400, see fa_brands_source.py -- carries its licence into the
 * copy compiled into the app. A font without them is unaffected.
 */
const PRESERVE_NAME_IDS = [0, 1, 2, 3, 4, 5, 6, 10, 13, 14];

export default function subsetFont(text: string | ((file: File) => string)) {
    return asyncTransform(async file => {
        if (!file.isBuffer()) {
            throw new Error('Only buffer file is supported!');
        }
        const bf = file as BufferFile;
        const buffer = bf.contents;
        bf.contents = await sf(buffer, typeof text === 'function' ? text(file) : text, {
            targetFormat: 'truetype',
            preserveNameIds: PRESERVE_NAME_IDS,
        });
        bf.extname = '.ttf';
    });
}