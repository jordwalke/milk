
open Longident;
open Parsetree;
open Ast_helper;
open Asttypes;
open TypeMap;

open Helpers;

// let makeJson = (kind, contents) => Exp.construct(Location.mknoloc(Ldot(Lident("Ezjsonm"), kind)), contents);
// let jsonObject = items => makeJson("Object", Some(makeList(items)));
// let jsonArray = items => makeJson("Array", Some(items));

let preamble = [];
let target = [%type: [ `Assoc(list((string, target)))
       | `Bool(bool)
       | `Float(float)
       | `Int(int)
       | `List(list(target))
       | `Null
       | `String(string)
       | `Intlit(string)
       | `Tuple(list(target))
       | `Variant((string, option(target)))
       ]
       ];

let sourceTransformer = (~source, ~transformers, ~input) => {
  let body = switch source {
  | DigTypes.NotFound => MakeSerializer.failer("Not found")
  | Public((moduleName, modulePath, name)) =>
    makeIdent(Lident(MakeSerializer.transformerName(~moduleName, ~modulePath, ~name)))
  | Builtin("array") =>
    [%expr (transformer, array) => `List(
      Belt.List.fromArray(Belt.Array.map(array, transformer))
    )]
  | Builtin("list") =>
    [%expr (transformer, list) => `List(Belt.List.map(list, transformer))]
  | Builtin("string") => [%expr s => `String(s)]
  | Builtin("bool") => [%expr b => `Bool(b)]
  | Builtin("unit") => [%expr b => `Null]
  | Builtin("int") => [%expr i => `Float(float_of_int(i))]
  | Builtin("float") => [%expr f => `Float(f)]
  | Builtin("option") =>  [%expr (transformer) => fun
    | None => `Null
    | Some(v) => transformer(v)]
  | Builtin(name) => failer("Builtin: " ++ name)
};
  switch transformers {
    | [] => MakeSerializer.maybeCall(body, input)
    | tr => MakeSerializer.maybeCall(Exp.apply(body, tr->Belt.List.map(expr => (Nolabel, expr))), input)
  }
};

let serializeTransformer =
  MakeSerializer.{
    wrapWithVersion: [%expr
      (version, payload) =>
        switch (payload) {
        | `Assoc(items) =>
          `Assoc(items @ [(schemaPropertyName, `Float(float_of_int(version)))])
        | _ => `List([`Float(float_of_int(version)), payload])
        }
    ],
    source: sourceTransformer,
    list: items => [%expr `List([%e items])],
    tuple: exps => [%expr `List([%e makeList(exps)])],
    record: (~renames, items) =>
      [%expr `Assoc([%e 
        makeList(items->Belt.List.map(((label, expr, _isOptional)) =>
          Exp.tuple([
            Exp.constant(Pconst_string(MakeDeserializer.getRename(~renames, label), None)),
            expr,
          ])
        ))
      ])],
    constructor: (~renames, name, args) =>
      [%expr `List([%e makeList(
            [
              [%expr `String(
                  [%e Exp.constant(Pconst_string(MakeDeserializer.getRename(~renames, name), None))]
                )
              ]
            ]
            @ args,
          )])],
  };

let declSerializer = MakeSerializer.decl(serializeTransformer);










let sourceTransformer = source => switch source {
  | DigTypes.NotFound => failer("Not found")
  | Public((moduleName, modulePath, name)) =>
    makeIdent(Lident(MakeDeserializer.transformerName(~moduleName, ~modulePath, ~name)))
  | Builtin("array") =>
    [%expr
      (transformer, array) => switch (array) {
        | `List(items) =>
          let rec loop = (collected, items) => switch items {
            | [] => Belt.Result.Ok(Belt.List.reverse(collected))
            | [one, ...rest] => switch (transformer(one)) {
              | Belt.Result.Error(error) => Belt.Result.Error(["array element", ...error])
              | Belt.Result.Ok(value) => loop([value, ...collected], rest)
            }
          };
          switch (loop([], items)) {
            | Belt.Result.Error(error) => Belt.Result.Error(error)
            | Belt.Result.Ok(value) => Belt.Result.Ok(Belt.List.toArray(value))
          }
        | _ => Belt.Result.Error(["expected an array"])
      }
    ];
  | Builtin("list") =>
    [%expr
      (transformer, list) => switch (list) {
        | `List(items) =>
          let rec loop = (collected, items) => switch items {
            | [] => Belt.Result.Ok(Belt.List.reverse(collected))
            | [one, ...rest] => switch (transformer(one)) {
              | Belt.Result.Error(error) => Belt.Result.Error(["list element", ...error])
              | Belt.Result.Ok(value) => loop([value, ...collected], rest)
            }
          };
          loop([], items)
        | _ => Belt.Result.Error(["expected an array"])
      }
    ];
  | Builtin("string") =>
    [%expr string => switch (string) {
      | `String(string) => Belt.Result.Ok(string)
      | _ => Error(["epected a string"])
    }]
  | Builtin("bool") => [%expr bool => switch (bool) {
    | `Bool(true) => Belt.Result.Ok(true)
    | `Bool(false) => Belt.Result.Ok(false)
    | _ => Belt.Result.Error(["Expected a bool"])
  }]
  | Builtin("int") =>
    [%expr number => switch (number) {
      | `Float(number) => Belt.Result.Ok(int_of_float(number))
      | _ => Error(["Expected a float"])
    }]
  | Builtin("float") => [%expr number => switch (number) {
    | `Float(number) => Belt.Result.Ok(number)
    | _ => Error(["Expected a float"])
  }]
  | Builtin("option") =>
    [%expr (transformer, option) => switch (option) {
      | `Null => Belt.Result.Ok(None)
      | _ => switch (transformer(option)) {
        | Belt.Result.Error(error) => Belt.Result.Error(["optional value", ...error])
        | Belt.Result.Ok(value) => Belt.Result.Ok(Some(value))
      }
    }]
  | Builtin(name) => failer("Builtin: " ++ name)
};

let rec makePatList = items => switch items {
  | [] => Pat.construct(Location.mknoloc(Lident("[]")), None)
  | [one, ...rest] => Pat.construct(Location.mknoloc(Lident("::")), Some(Pat.tuple([
    one, makePatList(rest)
  ])))
};

let deserializeTransformer = {
  MakeDeserializer.source: sourceTransformer,
  parseVersion: [%expr
    json => switch json {
      | `Assoc(items) => switch (items->Belt.List.getAssoc(schemaPropertyName, (==))) {
        | Some(`Float(schemaVersion)) => [@implicit_arity]Belt.Result.Ok((int_of_float(schemaVersion), json))
        | Some(_) => Belt.Result.Error("Invalid " ++ schemaPropertyName ++ " - expected number")
        | None => Belt.Result.Error("No " ++ schemaPropertyName)
      }
      | `List([`Float(version), payload]) => [@implicit_arity]Belt.Result.Ok((int_of_float(version), payload))
      | _ => Belt.Result.Error("Not wrapped in a version")
    }
  ],
  tuple: (value, patArgs, body) => [%expr json => switch ([%e value]) {
    | `List([%p makePatList(patArgs)]) => [%e body]
    | _ => Belt.Result.Error(["Expected array"])
  }],
  list: (transformer, list) => {
    [%expr
      switch ([%e list]) {
        | `List(items) =>
          let transformer = [%e transformer];
          let rec loop = (collected, items) => switch items {
            | [] => Belt.Result.Ok(Belt.List.reverse(collected))
            | [one, ...rest] => switch (transformer(one)) {
              | Belt.Result.Error(error) => Belt.Result.Error(["list item", ...error])
              | Belt.Result.Ok(value) => loop([value, ...collected], rest)
            }
          };
          loop([], items)
        | _ => Belt.Result.Error(["expected an array"])
      }
    ];

  },
  record: (~renames, ~typ, items) =>  {
    let body =
      MakeDeserializer.ok(
        Exp.constraint_(
        Exp.record(
          items->Belt.List.map(((label, _, _)) => (Location.mknoloc(Lident(label)), makeIdent(Lident("attr_" ++ label)))),
          None,
        ),
        typ
        )
      );
    let body = items->Belt.List.reduce(body, (body, (label, inner, isOptional)) => {
      let attrName = MakeDeserializer.getRename(~renames, label);
      let pat = Pat.var(Location.mknoloc("attr_" ++ label));
      {
        let%expr inner = ([%p pat]) => [%e body];
        switch (Belt.List.getAssoc(items, [%e MakeDeserializer.expString(attrName)], (==))) {
        | None =>
          if%e (isOptional) {
            %expr
            inner(None);
          } else {
            %expr
            Belt.Result.Error([[%e MakeDeserializer.expString("No attribute '" ++ attrName ++ "'")]]);
          }
        | Some(json) =>
          switch ([%e inner](json)) {
          | Belt.Result.Error(error) =>
            Belt.Result.Error([[%e MakeDeserializer.expString("attribute '" ++ attrName ++ "'")], ...error])
          | Belt.Result.Ok(data) => inner(data)
          }
        };
      };
    });
    [%expr record => switch (record) {
      | `Assoc(items) => [%e body]
      | _ => Belt.Result.Error(["Expected an object"])
    }]
  },
  variant: (~renames, constructors) => {

    let cases =
      constructors->Belt.List.map(((name, argCount, argTransformer)) => {
        let constrName = MakeDeserializer.getRename(~renames, name);
        let pat =
          [%pat? `List([%p
              makePatList([
                [%pat? `String([%p Pat.var(Location.mknoloc("tag"))])],
                ...MakeDeserializer.range(argCount, index =>
                     Pat.var(Location.mknoloc("arg" ++ string_of_int(index)))
                   ),
              ])
          ])];
        Exp.case(
          if (argCount == 0) {
            Pat.or_(
              pat,
              [%pat? `String(tag)]
            )
          } else { pat },
          ~guard=
            Exp.apply(
              makeIdent(Lident("=")),
              [
                (
                  Nolabel,
                  Exp.constant(Pconst_string(constrName, None)),
                ),
                (Nolabel, makeIdent(Lident("tag"))),
              ],
            ),
          argTransformer,
        );
      });

    Exp.fun_(
      Nolabel,
      None,
      Pat.var(Location.mknoloc("constructor")),
      Exp.match(
        makeIdent(Lident("constructor")),
        cases->Belt.List.concat([
          Exp.case([%pat? `List([`String(tag), ..._])], [%expr Belt.Result.Error(["Invalid constructor: " ++ tag])]),
          Exp.case(Pat.any(), MakeDeserializer.expError("Expected an array"))
        ])
      )
    )
  }
};


let declDeserializer = MakeDeserializer.decl(deserializeTransformer);
