// test_native.mjs — full loop: JS -> C (native module) -> gcc -> binary -> run
const cm = await import('E:/cmochicode/mochi_modules/cmochicode.dll');
console.log('cmochicode version:', cm.version());

// 1. transpile a real program through the module
const src = `
var hitung = (a, b = 10) => (a ?? 0) * 2 + b;
var user = { id: 7, nama: "Archana" };
var hasil = hitung(user?.id, 5);
console.log("Booting MochiBerry...");
console.log(\`User #\${user.id} (\${user.nama}) -> hasil: \${hasil}\`);
`;
const c = cm.compile(src);
console.log('--- generated C ---');
console.log(c);

// 2. write to a file and compile it with gcc
const fs = await import('E:/sevm/native/mochinode_poke.dll');
console.log('poke keys:', Object.keys(fs).slice(0,8));
