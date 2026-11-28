/* String API lengkap: transformasi, pencarian, split/concat, kode karakter. */
var s = "MochiBerry";
console.log("case", s.toUpperCase(), s.toLowerCase());
console.log("slice", s.slice(5, 10), s.slice(-5));
console.log("substr", s.substr(0, 5), "substring", s.substring(5, 10));
console.log("trim", "  pad  ".trim() + "|");
console.log("pad", "5".padStart(3, "0"), "9".padEnd(3, "0"));
console.log("repeat", "ab".repeat(3));

var csv = "one,two,three";
var parts = csv.split(",");
console.log("split", parts.length, parts[0], parts[2]);
console.log("split limit", csv.split(",", 2).join("|"));
console.log("join", parts.join(" + "));

console.log("search", s.indexOf("Berry"), s.lastIndexOf("M"), s.includes("chi"));
console.log("starts", s.startsWith("Mochi"), s.endsWith("Berry"));
console.log("char", s.charAt(0), "code", s.charCodeAt(0), s.at(-1));

console.log("replace", "a-b-c".replace("-", "+"));
console.log("replaceAll", "a-b-c".replaceAll("-", "+"));
console.log("concat", "x".concat("y", "z"));
console.log("chain", " Test ".trim().toUpperCase());

var word = "berry";
for (var i = 0; i < word.length; i++) {
    process.stdout.write(word.charAt(i));
}
console.log("");

var labels = ["id", "name", "value"];
for (var j = 0; j < labels.length; j++) {
    console.log(j + ": " + labels[j].padStart(6, "."));
}