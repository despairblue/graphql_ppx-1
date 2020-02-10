open Migrate_parsetree;
open Graphql_ppx_base;
open Source_pos;

open Output_bucklescript_utils;

let add_pos = (delimLength, base, pos) => {
  let (_, _, col) = Location.get_pos_info(conv_pos(base));
  {
    pos_fname: base.pos_fname,
    pos_lnum: base.pos_lnum + pos.line,
    pos_bol: 0,
    pos_cnum:
      switch (pos.line) {
      | 0 => delimLength + col + pos.col
      | _ => pos.col
      },
  };
};

let add_loc = (delimLength, base, span) => {
  loc_start: add_pos(delimLength, base.loc_start, fst(span)),
  loc_end: add_pos(delimLength, base.loc_start, snd(span)),
  loc_ghost: false,
};

let fmt_lex_err = err =>
  Graphql_lexer.(
    switch (err) {
    | Unknown_character(ch) => Printf.sprintf("Unknown character %c", ch)
    | Unexpected_character(ch) =>
      Printf.sprintf("Unexpected character %c", ch)
    | Unterminated_string => Printf.sprintf("Unterminated string literal")
    | Unknown_character_in_string(ch) =>
      Printf.sprintf("Unknown character in string literal: %c", ch)
    | Unknown_escape_sequence(s) =>
      Printf.sprintf("Unknown escape sequence in string literal: %s", s)
    | Unexpected_end_of_file => Printf.sprintf("Unexpected end of query")
    | Invalid_number => Printf.sprintf("Invalid number")
    }
  );

let global_records = () => Ppx_config.records();
let global_template_tag = () => Ppx_config.template_tag();
let global_definition = () => Ppx_config.definition();
let legacy = () => Ppx_config.legacy();

let fmt_parse_err = err =>
  Graphql_parser.(
    switch (err) {
    | Unexpected_token(t) =>
      Printf.sprintf("Unexpected token %s", Graphql_lexer.string_of_token(t))
    | Unexpected_end_of_file => "Unexpected end of query"
    | Lexer_error(err) => fmt_lex_err(err)
    }
  );

let make_error_expr = (loc, message) => {
  Ast_406.(
    Ast_mapper.extension_of_error(Location.error(~loc, message))
    |> Ast_helper.Exp.extension(~loc)
  );
};

let extract_schema_from_config = config_fields => {
  open Ast_406;
  open Asttypes;
  open Parsetree;

  let maybe_schema_field =
    try(
      Some(
        List.find(
          config_field =>
            switch (config_field) {
            | (
                {txt: Longident.Lident("schema"), _},
                {pexp_desc: Pexp_constant(Pconst_string(_, _)), _},
              ) =>
              true
            | _ => false
            },
          config_fields,
        ),
      )
    ) {
    | _ => None
    };

  switch (maybe_schema_field) {
  | Some((_, {pexp_desc: Pexp_constant(Pconst_string(schema_name, _)), _})) =>
    Some(schema_name)
  | _ => None
  };
};

let extract_bool_from_config = (name, config_fields) => {
  open Ast_406;
  open Asttypes;
  open Parsetree;

  let maybe_field_value =
    try(
      Some(
        List.find(
          config_field =>
            switch (config_field) {
            | (
                {txt: Longident.Lident(matched_name), _},
                {
                  pexp_desc:
                    Pexp_construct({txt: Longident.Lident(_value)}, _),
                  _,
                },
              )
                when matched_name == name =>
              true
            | _ => false
            },
          config_fields,
        ),
      )
    ) {
    | _ => None
    };

  switch (maybe_field_value) {
  | Some((
      _,
      {pexp_desc: Pexp_construct({txt: Longident.Lident(value)}, _), _},
    )) =>
    switch (value) {
    | "true" => Some(true)
    | "false" => Some(false)
    | _ => None
    }
  | _ => None
  };
};

let extract_template_tag_from_config = config_fields => {
  open Ast_406;
  open Asttypes;
  open Parsetree;

  let maybe_template_tag_field =
    try(
      Some(
        List.find(
          config_field =>
            switch (config_field) {
            | (
                {txt: Longident.Lident("templateTag"), _},
                {pexp_desc: Pexp_ident({txt: _}), _},
              ) =>
              true
            | _ => false
            },
          config_fields,
        ),
      )
    ) {
    | _ => None
    };

  switch (maybe_template_tag_field) {
  | Some((_, {pexp_desc: Pexp_ident({txt: lident})})) =>
    Some(
      Longident.flatten(lident)
      |> List.fold_left(
           (acc, elem) =>
             if (acc == "") {
               elem;
             } else {
               acc ++ "." ++ elem;
             },
           "",
         ),
    )
  | _ => None
  };
};

let extract_records_from_config = extract_bool_from_config("records");
let extract_inline_from_config = extract_bool_from_config("inline");
let extract_definition_from_config = extract_bool_from_config("definition");
let extract_tagged_template_config =
  extract_bool_from_config("taggedTemplate");

type query_config = {
  schema: option(string),
  records: option(bool),
  inline: option(bool),
  definition: option(bool),
  template_tag: option(string),
  tagged_template: option(bool),
};

let get_query_config = fields => {
  {
    schema: extract_schema_from_config(fields),
    records: extract_records_from_config(fields),
    inline: extract_inline_from_config(fields),
    definition: extract_definition_from_config(fields),
    template_tag: extract_template_tag_from_config(fields),
    tagged_template: extract_tagged_template_config(fields),
  };
};
let empty_query_config = {
  schema: None,
  records: None,
  inline: None,
  definition: None,
  template_tag: None,
  tagged_template: None,
};

let rewrite_query =
    (
      ~query_config: query_config,
      ~loc,
      ~delim,
      ~query,
      ~module_definition,
      (),
    ) => {
  open Ast_406;
  open Ast_helper;

  let lexer = Graphql_lexer.make(query);
  let delimLength =
    switch (delim) {
    | Some(s) => 2 + String.length(s)
    | None => 1
    };
  switch (Graphql_lexer.consume(lexer)) {
  | Result.Error(e) =>
    raise(
      Location.Error(
        Location.error(
          ~loc=add_loc(delimLength, loc, e.span) |> conv_loc,
          fmt_lex_err(e.item),
        ),
      ),
    )
  | Result.Ok(tokens) =>
    let parser = Graphql_parser.make(tokens);
    switch (Graphql_parser_document.parse_document(parser)) {
    | Result.Error(e) =>
      raise(
        Location.Error(
          Location.error(
            ~loc=add_loc(delimLength, loc, e.span) |> conv_loc,
            fmt_parse_err(e.item),
          ),
        ),
      )
    | Result.Ok(document) =>
      let config = {
        Generator_utils.map_loc: add_loc(delimLength, loc),
        delimiter: delim,
        full_document: document,
        records:
          switch (query_config.records) {
          | Some(value) => value
          | None => global_records()
          },
        inline:
          switch (query_config.inline) {
          | Some(value) => value
          | None => false
          },
        definition:
          switch (query_config.definition) {
          | Some(value) => value
          | None => global_definition()
          },
        legacy: legacy(),
        /*  the only call site of schema, make it lazy! */
        schema: Lazy.force(Read_schema.get_schema(query_config.schema)),
        template_tag:
          switch (query_config.tagged_template, query_config.template_tag) {
          | (_, Some(value)) => Some(value)
          | (Some(false), _) => None
          | (_, None) => global_template_tag()
          },
      };
      switch (Validations.run_validators(config, document)) {
      | Some(errs) =>
        let errs =
          errs
          |> List.map(((loc, msg)) => {
               let loc = conv_loc(loc);
               %stri
               [%e make_error_expr(loc, msg)];
             });
        [errs];
      | None =>
        Result_decoder.unify_document_schema(config, document)
        |> Output_bucklescript_module.generate_modules(
             config,
             module_definition,
           )
      };
    };
  };
};

// Default configuration
let () =
  Ppx_config.(
    set_config({
      verbose_logging: false,
      output_mode: Ppx_config.String,
      verbose_error_handling:
        switch (Sys.getenv("NODE_ENV")) {
        | "production" => false
        | _ => true
        | exception Not_found => true
        },
      apollo_mode: false,
      schema_file: "graphql_schema.json",
      root_directory: Sys.getcwd(),
      raise_error_with_loc: (loc, message) => {
        let loc = conv_loc(loc);
        raise(Location.Error(Location.error(~loc, message)));
      },
      records: false,
      legacy: false,
      template_tag: None,
      definition: true,
    })
  );

let mapper = (_config, _cookies) => {
  Ast_406.(
    Ast_mapper.(
      Parsetree.(
        Asttypes.{
          ...default_mapper,
          module_expr: (mapper, mexpr) =>
            switch (mexpr) {
            | {pmod_desc: Pmod_extension(({txt: "graphql", loc}, pstr)), _} =>
              switch (pstr) {
              | PStr([
                  {
                    pstr_desc:
                      Pstr_eval(
                        {
                          pexp_loc: loc,
                          pexp_desc:
                            Pexp_constant(Pconst_string(query, delim)),
                          _,
                        },
                        _,
                      ),
                    _,
                  },
                  {
                    pstr_desc:
                      Pstr_eval(
                        {pexp_desc: Pexp_record(fields, None), _},
                        _,
                      ),
                    _,
                  },
                ]) =>
                Ast_helper.(
                  Mod.mk(
                    Pmod_structure(
                      List.concat(
                        rewrite_query(
                          ~query_config=get_query_config(fields),
                          ~loc=conv_loc_from_ast(loc),
                          ~delim,
                          ~query,
                          ~module_definition=true,
                          (),
                        ),
                      ),
                    ),
                  )
                )
              | PStr([
                  {
                    pstr_desc:
                      Pstr_eval(
                        {
                          pexp_loc: loc,
                          pexp_desc:
                            Pexp_constant(Pconst_string(query, delim)),
                          _,
                        },
                        _,
                      ),
                    _,
                  },
                ]) =>
                Ast_helper.(
                  Mod.mk(
                    Pmod_structure(
                      List.concat(
                        rewrite_query(
                          ~query_config=empty_query_config,
                          ~loc=conv_loc_from_ast(loc),
                          ~delim,
                          ~query,
                          ~module_definition=true,
                          (),
                        ),
                      ),
                    ),
                  )
                )
              | _ =>
                raise(
                  Location.Error(
                    Location.error(
                      ~loc,
                      "[%graphql] accepts a string, e.g. [%graphql {| { query |}]",
                    ),
                  ),
                )
              }
            | other => default_mapper.module_expr(mapper, other)
            },
          structure: (mapper, struc) => {
            struc
            |> List.fold_left(
                 acc =>
                   fun
                   | {
                       pstr_desc:
                         Pstr_eval(
                           {
                             pexp_desc:
                               Pexp_extension(({txt: "graphql", loc}, pstr)),
                           },
                           _,
                         ),
                     }
                   | {
                       pstr_desc:
                         Pstr_value(
                           _,
                           [
                             {
                               pvb_pat: {ppat_desc: _},
                               pvb_expr: {
                                 pexp_desc:
                                   Pexp_extension((
                                     {txt: "graphql", loc},
                                     pstr,
                                   )),
                               },
                             },
                           ],
                         ),
                     } =>
                     switch (pstr) {
                     | PStr([
                         {
                           pstr_desc:
                             Pstr_eval(
                               {
                                 pexp_loc: loc,
                                 pexp_desc:
                                   Pexp_constant(
                                     Pconst_string(query, delim),
                                   ),
                                 _,
                               },
                               _,
                             ),
                           _,
                         },
                         {
                           pstr_desc:
                             Pstr_eval(
                               {pexp_desc: Pexp_record(fields, None), _},
                               _,
                             ),
                           _,
                         },
                       ]) =>
                       List.append(
                         acc,
                         List.concat(
                           rewrite_query(
                             ~query_config=get_query_config(fields),
                             ~loc=conv_loc_from_ast(loc),
                             ~delim,
                             ~query,
                             ~module_definition=false,
                             (),
                           ),
                         ),
                       )
                     | PStr([
                         {
                           pstr_desc:
                             Pstr_eval(
                               {
                                 pexp_loc: loc,
                                 pexp_desc:
                                   Pexp_constant(
                                     Pconst_string(query, delim),
                                   ),
                                 _,
                               },
                               _,
                             ),
                           _,
                         },
                       ]) =>
                       List.append(
                         acc,
                         List.concat(
                           rewrite_query(
                             ~query_config=empty_query_config,
                             ~loc=conv_loc_from_ast(loc),
                             ~delim,
                             ~query,
                             ~module_definition=false,
                             (),
                           ),
                         ),
                       )
                     | _ =>
                       raise(
                         Location.Error(
                           Location.error(
                             ~loc,
                             "[%graphql] accepts a string, e.g. [%graphql {| { query |}]",
                           ),
                         ),
                       )
                     }
                   | other =>
                     List.append(
                       acc,
                       [default_mapper.structure_item(mapper, other)],
                     ),
                 [],
               );
          },
        }
      )
    )
  );
};

let args = [
  (
    "-verbose",
    Arg.Unit(
      () =>
        Ppx_config.update_config(current =>
          {...current, verbose_logging: true}
        ),
    ),
    "Defines if logging should be verbose or not",
  ),
  (
    "-apollo-mode",
    Arg.Unit(
      () =>
        Ppx_config.update_config(current => {...current, apollo_mode: true}),
    ),
    "Defines if apply Apollo specific code generation",
  ),
  (
    "-schema",
    Arg.String(
      schema_file =>
        Ppx_config.update_config(current => {...current, schema_file}),
    ),
    "<path/to/schema.json>",
  ),
  (
    "-ast-out",
    Arg.Bool(
      ast_out =>
        Ppx_config.update_config(current =>
          {
            ...current,
            output_mode: ast_out ? Ppx_config.Apollo_AST : Ppx_config.String,
          }
        ),
    ),
    "Defines if output string or AST",
  ),
  (
    "-o",
    Arg.Unit(
      () =>
        Ppx_config.update_config(current =>
          {...current, verbose_error_handling: true}
        ),
    ),
    "Verbose error handling. If not defined NODE_ENV will be used",
  ),
  (
    "-records",
    Arg.Unit(
      () => Ppx_config.update_config(current => {...current, records: true}),
    ),
    "Compile to records instead of objects (experimental)",
  ),
  (
    "-legacy",
    Arg.Unit(
      () => Ppx_config.update_config(current => {...current, records: false}),
    ),
    "Legacy mode (make and makeWithVariables)",
  ),
  (
    "-no-definition",
    Arg.Unit(
      () =>
        Ppx_config.update_config(current => {...current, definition: false}),
    ),
    "Legacy mode (make and makeWithVariables)",
  ),
  (
    "-template-tag",
    Arg.String(
      template_tag =>
        Ppx_config.update_config(current =>
          {...current, template_tag: Some(template_tag)}
        ),
    ),
    "graphql",
  ),
];

let () =
  Migrate_parsetree.(
    Driver.register(~name="graphql", ~args, Versions.ocaml_406, mapper)
  );
