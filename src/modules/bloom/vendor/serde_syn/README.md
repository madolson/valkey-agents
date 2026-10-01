[![Build Status](https://gitlab.com/samsartor/serde_syn/badges/master/build.svg)](https://gitlab.com/samsartor/serde_syn/commits/master) [![Documentation](https://img.shields.io/badge/docs-master%20(0.1.0)-blue.svg)](https://samsartor.gitlab.io/serde_syn/serde_syn/index.html)

# serde_syn

serde_syn is a serde backend for parsing Rust syntax inside procedural
macros. For example, you can deserialize parameters for a custom derive out
of attributes and directly into structs. The goal is to eliminate the
parsing boilerplate that goes into writing procedural macros.

The interface to serde_syn is fairly minimal. Here are a few ways to use it:
- syn requires a
  [`Parser`](https://docs.rs/syn/1.0/syn/parse/trait.Parser.html)
  implementation to process syntax inside already parsed/visited
  [attributes](https://docs.rs/syn/1.0/syn/struct.Attribute.html#method.parse_args_with).
  The [`parser`] function creates exactly that!
- If you are working directly with `proc_macro`/`proc_macro2` token streams
  or strings, you should also use [`parser`].
- If you are [implementing syn's `Parse`
  trait](https://docs.rs/syn/1.0/syn/parse/index.html) yourself, you should
  use the [`from_stream`] function which takes in a
  [`ParseStream`](https://docs.rs/syn/1.0/syn/parse/type.ParseStream.html).

Lots of pre-made configurations exist inside the [`config`] module for
common syntaxes (JSON-like, attribute-like, expression-like, etc) or you can
combine flags to build your own.

### Example derive implementation
Here you can see a simple derive macro implementation. For more examples,
see [the examples
directory](https://gitlab.com/samsartor/serde_syn/tree/master/examples).

```rust
#
#
/// The format of `named` attributes.
#[derive(Deserialize)]
struct Props {
    rename: Option<String>, // #[named(rename="hello")]
    lowercase: Option<()>,  // #[named(lowercase)]
}

#[proc_macro_derive(NamedType, attributes(named))]
pub fn my_macro(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let mut name = input.ident.to_string();

    for attr in input.attrs.iter().filter(|a| a.path.is_ident("named")) {
        let parser = parser::<Props>(config::RUSTY_META);
        let props = match attr.parse_args_with(parser) {
            Ok(props) => props,
            Err(err) => return err.to_compile_error().into(),
        };

        if let Some(rename) = props.rename { name = rename; }
        if props.lowercase.is_some() { name = name.to_lowercase(); }
    }

    let ident = &input.ident;
    (quote! {
        impl NamedType for #ident {
            fn name() -> &'static str { #name }
        }
    }).into()
}
```

### Error handling
Deserialization errors are automatically assigned a "span" (the area of the
source code that could not be parsed) before being returned from [`parser`]
and [`from_stream`] as ordinary
[`syn::Error`](https://docs.rs/syn/1.0/syn/struct.Error.html)s. When that
error is reported to the Rust compiler, the correct regions of code will be
highlighted:

```
error: unknown field `lowrcase`, expected `rename` or `lowercase`
  --> named_type.rs:4:13
   |
4  |     #[named(lowrcase)]
   |             ^^^^^^^^
```

If you use [`Deserializer`] directly, serde_syn will do its best to assign a
span but it is always possible to create an error with no span using serde's
required `custom` function.


### Limitations
serde_syn is early in development and so still has some gotchyas. For
example, serde_syn will throw an error if you try to deserialize into a
[serde_json `Value`](https://docs.serde.rs/serde_json/value/enum.Value.html)
since it doesn't yet support self-description.

If you find any bugs, have any ideas, or wind up with free time to help
random open source projects, please drop by [the
repository](https://gitlab.com/samsartor/serde_syn/).
