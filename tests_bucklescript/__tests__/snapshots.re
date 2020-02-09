open Jest;
open Expect;

type options = {cwd: string};
type buffer;

[@bs.module "path"] external resolve: (string, string) => string = "resolve";

[@bs.module "child_process"]
external execSync: (string, options) => buffer = "execSync";
[@bs.module "fs"]
external readdirSync: string => array(string) = "readdirSync";
[@bs.module "os"] external platform: unit => string = "platform";

[@bs.val] external dirname: string = "__dirname";
[@bs.send] external toString: buffer => string = "toString";

let win = platform() == "win32";
let refmt =
  execSync(
    "esy @406 build echo \"#{@opam/reason.bin / }refmt\"",
    {cwd: resolve(dirname, "../..")},
  )
  |> toString
  |> Js.String.trim;

let run_ppx = (path, opts) => {
  execSync(
    (win ? "type " : "cat ")
    ++ path
    ++ " | "
    ++ refmt
    ++ " --parse re --print binary | ../_build/default/src/bucklescript_bin/bin.exe -schema ../graphql_schema.json "
    ++ opts
    ++ (win ? " - -o -" : " /dev/stdin /dev/stdout")
    ++ " | "
    ++ refmt
    ++ " --parse binary --print ml --interface false",
    {cwd: resolve(dirname, "..")},
  )
  |> toString;
};

let tests = readdirSync("operations");

describe("Objects", () => {
  tests
  |> Array.map(t => {
       test(t, () =>
         expect(run_ppx("operations/" ++ t, "")) |> toMatchSnapshot
       )
     })
  |> ignore
});

describe("Records", () => {
  tests
  |> Array.map(t => {
       test(t, () =>
         expect(run_ppx("operations/" ++ t, "-records")) |> toMatchSnapshot
       )
     })
  |> ignore
});

describe("Records & Legacy", () => {
  tests
  |> Array.map(t => {
       test(t, () =>
         expect(run_ppx("operations/" ++ t, "-records -legacy"))
         |> toMatchSnapshot
       )
     })
  |> ignore
});

describe("Records & No definition", () => {
  tests
  |> Array.map(t => {
       test(t, () =>
         expect(run_ppx("operations/" ++ t, "-records -no-definition"))
         |> toMatchSnapshot
       )
     })
  |> ignore
});
