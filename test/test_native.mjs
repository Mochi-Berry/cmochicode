// test_native.mjs — full loop: JS -> C (native module) -> gcc -> binary -> run
const cm = await import('E:/cmochicode/mochi_modules/lib/cmochicode_x86_64.dll');
console.log('cmochicode version:', cm.version());

const src = `
var a = [1, 2, 3, 4];
var d = a.map(x => x * 2);
console.log("map", d);
var s = a.reduce((acc, x) => acc + x, 0);
console.log("reduce", s);
var user = { id: 7, nama: "Archana", active: true };
var keys = "";
for (var k in user) { keys = keys + k + " "; }
console.log("keys", keys);
var hitung = (a, b = 10) => (a ?? 0) * 2 + b;
var hasil = hitung(user?.id, 5);
console.log("Booting MochiBerry...");
console.log(\`User #\${user.id} (\${user.nama}) -> hasil: \${hasil}\`);
`;
const c = cm.compile(src);
console.log('--- generated C ---');
console.log(c);

// write + gcc + run
const fs = await import('E:/sevm/native/mochinode_poke.dll');
fs.writeFileSync('E:/cmochicode/build/mod_test.c', c);
const { execSync } = await import('E:/sevm/native/mochinode_poke.dll');
execSync('gcc -std=c99 -O2 -s -I E:/cmochicode/runtime E:/cmochicode/build/mod_test.c -o E:/cmochicode/build/mod_test.exe -lm');
console.log('--- module-compiled binary output ---');
console.log(execSync('E:/cmochicode/build/mod_test.exe').toString());
