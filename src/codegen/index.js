// index.js — assemble the CGen oracle from per-concern parts.
require('./expr');
require('./stmt');
const { CGen } = require('./core');
const { cStr } = require('./helpers');

module.exports = { CGen, cStr };
