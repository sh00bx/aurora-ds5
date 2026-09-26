import {dest, src} from "gulp";
import binheader from "./binheader";
import codepoints from "./codepoints";
import rename from "gulp-rename";
import subsetFont from "./gulp-subset-font";
import minimist, {ParsedArgs} from "minimist";
import symheader from "./symheader";
import asyncTransform from "./async-transform";
import {snakeCase} from "change-case";

declare interface Args extends ParsedArgs {
    input: string | string[];
    output: string;
}

const options = minimist<Args>(process.argv.slice(2));

/* The macro prefix of each icon font's symbols header. A font that is not
 * listed here stops the build rather than guessing one. */
const SYMBOL_PREFIX: Record<string, string> = {
    'MaterialIcons-Regular': 'MAT',
    'fa-brands-400': 'FA',
};

function codepointsMetadata(file: any): Record<string, number> {
    return file.codepoints;
}

/* The icon fonts are loaded as ONE fontset, each a FreeType fallback of the one
 * before (util/font.c), and a label asks the first font that has a codepoint.
 * A codepoint two subsets share would therefore draw only the first font's
 * glyph, whichever icon the symbol was meant to be -- so no two may share one. */
function disjointCodepoints() {
    const owner = new Map<number, string>();
    return asyncTransform(async file => {
        for (const [name, cp] of Object.entries(codepointsMetadata(file))) {
            const other = owner.get(cp);
            if (other !== undefined && other !== file.stem) {
                throw new Error(`U+${cp.toString(16).toUpperCase()} (${name}) is in both ${other} and ${file.stem}`);
            }
            owner.set(cp, file.stem);
        }
    });
}

async function iconfont() {
    return src(options.input)
        .pipe(codepoints())
        .pipe(disjointCodepoints())
        .pipe(subsetFont(file => String.fromCodePoint(...Object.values(codepointsMetadata(file)))))
        .pipe(binheader({naming: 'snake_case', prefix: 'ttf'}))
        .pipe(rename(file => {
            file.basename = `${snakeCase(file.basename)}_ttf`;
        }))
        .pipe(dest(options.output));
}

async function symlist() {
    return src(options.input)
        .pipe(codepoints())
        .pipe(disjointCodepoints())
        .pipe(symheader({
            prefix: file => {
                const prefix = SYMBOL_PREFIX[file.stem];
                if (!prefix) {
                    throw new Error(`No symbol prefix for ${file.stem}; add it to SYMBOL_PREFIX`);
                }
                return prefix;
            }
        }))
        .pipe(rename(file => {
            file.basename = `${snakeCase(file.basename)}_symbols`;
        }))
        .pipe(dest(options.output));
}

async function binaries() {
    return src(options.input)
        .pipe(binheader({naming: 'snake_case', prefix: 'res'}))
        .pipe(dest(options.output));
}

exports['iconfont'] = iconfont;
exports['symlist'] = symlist;
exports['binaries'] = binaries;
