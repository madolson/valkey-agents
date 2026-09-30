//! serde_syn is a serde backend for parsing Rust syntax inside procedural
//! macros. For example, you can deserialize parameters for a custom derive out
//! of attributes and directly into structs. The goal is to eliminate the
//! parsing boilerplate that goes into writing procedural macros.
//!
//! The interface to serde_syn is fairly minimal. Here are a few ways to use it:
//! - syn requires a
//!   [`Parser`](https://docs.rs/syn/1.0/syn/parse/trait.Parser.html)
//!   implementation to process syntax inside already parsed/visited
//!   [attributes](https://docs.rs/syn/1.0/syn/struct.Attribute.html#method.parse_args_with).
//!   The [`parser`] function creates exactly that!
//! - If you are working directly with `proc_macro`/`proc_macro2` token streams
//!   or strings, you should also use [`parser`].
//! - If you are [implementing syn's `Parse`
//!   trait](https://docs.rs/syn/1.0/syn/parse/index.html) yourself, you should
//!   use the [`from_stream`] function which takes in a
//!   [`ParseStream`](https://docs.rs/syn/1.0/syn/parse/type.ParseStream.html).
//!
//! Lots of pre-made configurations exist inside the [`config`] module for
//! common syntaxes (JSON-like, attribute-like, expression-like, etc) or you can
//! combine flags to build your own.
//!
//! ## Example derive implementation
//! Here you can see a simple derive macro implementation. For more examples,
//! see [the examples
//! directory](https://gitlab.com/samsartor/serde_syn/tree/master/examples).
//!
//! ```
//! # extern crate proc_macro;
//! #
//! # use serde_syn::{parser, config};
//! # use syn::{parse_macro_input, DeriveInput};
//! # use serde_derive::{Deserialize};
//! # use proc_macro::TokenStream;
//! # use proc_macro2::Span;
//! # use quote::quote;
//! #
//! /// The format of `named` attributes.
//! #[derive(Deserialize)]
//! struct Props {
//!     rename: Option<String>, // #[named(rename="hello")]
//!     lowercase: Option<()>,  // #[named(lowercase)]
//! }
//!
//! # const IGNORE_TOKENS: &str = stringify! {
//! #[proc_macro_derive(NamedType, attributes(named))]
//! # };
//! pub fn my_macro(input: TokenStream) -> TokenStream {
//!     let input = parse_macro_input!(input as DeriveInput);
//!     let mut name = input.ident.to_string();
//!
//!     for attr in input.attrs.iter().filter(|a| a.path.is_ident("named")) {
//!         let parser = parser::<Props>(config::RUSTY_META);
//!         let props = match attr.parse_args_with(parser) {
//!             Ok(props) => props,
//!             Err(err) => return err.to_compile_error().into(),
//!         };
//!
//!         if let Some(rename) = props.rename { name = rename; }
//!         if props.lowercase.is_some() { name = name.to_lowercase(); }
//!     }
//!
//!     let ident = &input.ident;
//!     (quote! {
//!         impl NamedType for #ident {
//!             fn name() -> &'static str { #name }
//!         }
//!     }).into()
//! }
//! ```
//!
//! ## Error handling
//! Deserialization errors are automatically assigned a "span" (the area of the
//! source code that could not be parsed) before being returned from [`parser`]
//! and [`from_stream`] as ordinary
//! [`syn::Error`](https://docs.rs/syn/1.0/syn/struct.Error.html)s. When that
//! error is reported to the Rust compiler, the correct regions of code will be
//! highlighted:
//!
//! ```text
//! error: unknown field `lowrcase`, expected `rename` or `lowercase`
//!   --> named_type.rs:4:13
//!    |
//! 4  |     #[named(lowrcase)]
//!    |             ^^^^^^^^
//! ```
//!
//! If you use [`Deserializer`] directly, serde_syn will do its best to assign a
//! span but it is always possible to create an error with no span using serde's
//! required `custom` function.
//!
//!
//! ## Limitations
//! serde_syn is early in development and so still has some gotchyas. For
//! example, serde_syn will throw an error if you try to deserialize into a
//! [serde_json `Value`](https://docs.serde.rs/serde_json/value/enum.Value.html)
//! since it doesn't yet support self-description.
//!
//! If you find any bugs, have any ideas, or wind up with free time to help
//! random open source projects, please drop by [the
//! repository](https://gitlab.com/samsartor/serde_syn/).


mod error;
mod de;
pub mod config;

pub use error::Error;
pub use de::*;
pub use config::Config;

use syn::Error as SynError;
use syn::parse::{Parser, ParseStream};
use serde::{de::Deserialize, de::DeserializeOwned};

/// Create a Syn `Parser` which can deserialize the type `T` from a token
/// stream or string.
///
///
/// ```
/// # use serde_syn::{parser, config};
/// # use syn::parse::Parser;
/// # use quote::quote;
/// let from_string: Vec<String> = parser(config::RUSTY)
///     .parse_str("[\"Hello\", \"World\"]")?;
///
/// let from_tokens: Vec<String> = parser(config::RUSTY)
///     .parse2(quote! { ["Hello", "World"] })?;
/// # Ok::<(), syn::Error>(())
/// ```
pub fn parser<T: DeserializeOwned>(config: Config) -> impl Parser<Output=T> {
    // Really odd lifetimes here. Somehow this combination of annotations and
    // closures compiles.
    move |stream: ParseStream| -> syn::Result<T> {
        from_stream::<T>(config, stream)
    }
}

/// Deserialize the type `T` from Rust syntax. This will almost always be used
/// from inside a `Parse` implementation. To deserialize a token stream or
/// string, consider using [`parser`].
///
/// For example, to parse attributes of the form
/// ```text
/// #[text("Hello"), count(10)]
/// #[count = 50]
/// #[{ text: "Hello" }]
/// ```
///
/// you could create a procedual macro that uses `from_stream`:
/// ```
/// # extern crate proc_macro;
/// use proc_macro::TokenStream;
/// use syn::{parse_macro_input, bracketed, Token};
/// use syn::parse::{Parse, ParseStream};
/// use serde_derive::Deserialize;
/// use serde_syn::{from_stream, config};
///
/// #[derive(Deserialize)]
/// struct MyMacroAttr {
///     text: Option<String>,
///     #[serde(default)]
///     count: usize,
/// }
///
/// impl Parse for MyMacroAttr {
///     fn parse(input: ParseStream) -> syn::Result<Self> {
///         let pound_token: Token![#] = input.parse()?;
///         let content;
///         let bracket_token = bracketed!(content in input);
///         from_stream(config::RUSTY_META, &content)
///     }
/// }
///
/// # const IGNORE_TOKENS: &str = stringify! {
/// #[proc_macro]
/// # };
/// pub fn my_macro(input: TokenStream) -> TokenStream {
///     let attributes = parse_macro_input!(input as MyMacroAttr);
///
///     /* your macro here */
/// #   TokenStream::new()
/// }
/// ```
pub fn from_stream<'a, T>(config: Config, tokens: ParseStream<'a>)
    -> Result<T, SynError>
    where T: Deserialize<'a>
{
    let span = tokens.cursor().span();
    Deserialize::deserialize(Deserializer::new(config, tokens))
        .map_err(|e| e.to_syn(span))
}
