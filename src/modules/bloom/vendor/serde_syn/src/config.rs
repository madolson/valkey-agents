//! Configure serde_syn to parse different styles of syntax.

use bitflags::bitflags;

/// A structure for setting parser options. Defaults to `RUSTY`.
#[derive(Copy, Clone, Debug)]
pub struct Config {
    /// Flags relating to structs.
    pub structs: KvOpts,

    /// Flags relating to maps.
    pub maps: KvOpts,

    /// Other flags.
    pub global: GlobalOpts,
}

impl Default for Config {
    fn default() -> Config { RUSTY }
}

bitflags! {
    /// Options for parsing structs and maps.
    pub struct KvOpts: u8 {
        /// Allow blocks of `$key: $value` where `$key` is an identifier.
        /// ```text
        /// {
        ///     text: "Hello",
        ///     number: 7,
        /// }
        /// ```
        const STRUCT_LIKE = 0b00000001;

        /// Allow blocks of `$key => $value` where `$key` is any type.
        /// ```text
        /// {
        ///     text => "Hello",
        ///     "number" => 7,
        ///     [1, 2, 3] => "One, Two, Three",
        /// }
        /// ```
        const MATCH_LIKE = 0b00000010;

        /// Allow blocks of `$key = $value` where `$key` is an identifier.
        /// ```text
        /// {
        ///     text = "Hello",
        ///     number = 7,
        /// }
        /// ```
        const STATEMENT_LIKE = 0b00000100;

        /// Should arrays of 2-tuples be interpreted as mappings from keys to
        /// values?
        /// ```text
        /// [
        ///     ("text", "Hello"),
        ///     ("number", 7),
        ///     ([1, 2, 3], "One, Two, Three"),
        /// ]
        /// ```
        const TUPLE_ARRAY = 0b00001000;

        /// Allow groups of `$key($value)` where `$key` is an identifier. Note
        /// that attribute-like groups can be nested without extra parentheses.
        /// ```text
        /// (
        ///     text(Hello),
        ///     number(7),
        /// )
        /// ```
        const ATTRIBUTE_LIKE = 0b00010000;

        /// Should struct-like inputs allow strings in identifier position?
        /// ```text
        /// {
        ///     "text": "Hello",
        ///     "number": 7,
        /// }
        /// ```
        const STRING_IDENTS = 0b00100000;

        /// Can the value of a unit field be left off?
        /// (e.g. `skip` instead of `skip = ()`)
        const BARE_UNIT = 0b00000100;
    }
}

bitflags! {
    /// Other parsing options.
    pub struct GlobalOpts: u8 {
        /// Can structs be unnamed?
        const BARE_STRUCTS = 0b00000001;

        /// Can enums be unnamed
        /// (e.g. `Varient { }` instead of `Enum::Varient { }`)
        const BARE_ENUMS = 0b00000010;

        /// Can Some be left off?
        /// (e.g. `42` instead of `Some(42)`)
        const BARE_OPTION = 0b00010000;

        /// Can braces be left off the root map/list/struct?
        const UNGROUPED_ROOT = 0b00000100;

        /// Can strings be parsed as identifier values?
        const STRING_IDENTS = 0b00001000;
     }
}

/// A JSON-like configuration. Allows unquoted keys, tuple maps, and typed
/// values. Also uses parentheses for tuples instead of square braces.
/// ```text
/// {
///     "text": "Hello, World!",
///     "hashmap": [
///         ([1], 1),
///         ([1, 2], 2),
///         ([1, 2, 3], 3),
///     ],
///     "oneof": MyVariant {
///         "data": ["a", "b", "c"],
///     },
/// }
/// ```
pub const JSONY: Config = Config {
    structs: KvOpts::from_bits_truncate(
        KvOpts::STRING_IDENTS.bits |
        KvOpts::STRUCT_LIKE.bits),
    maps: KvOpts::from_bits_truncate(
        KvOpts::STRING_IDENTS.bits |
        KvOpts::STRUCT_LIKE.bits |
        KvOpts::TUPLE_ARRAY.bits),
    global: GlobalOpts::from_bits_truncate(
        GlobalOpts::BARE_STRUCTS.bits |
        GlobalOpts::BARE_ENUMS.bits |
        GlobalOpts::STRING_IDENTS.bits |
        GlobalOpts::BARE_OPTION.bits),
};

/// A Rust-like configuration which has very permissive syntax.
/// ```text
/// Input {
///     text = "Hello, World!",
///     hashmap = {
///         [1] => 1,
///         [1, 2] => 2,
///         [1, 2, 3] => 3,
///     },
///     oneof = MyEnum::MyVariant {
///         data: ["a", "b", "c"],
///     },
/// }
/// ```
pub const RUSTY: Config = Config {
    structs: KvOpts::from_bits_truncate(
        KvOpts::STRUCT_LIKE.bits |
        KvOpts::STATEMENT_LIKE.bits),
    maps: KvOpts::from_bits_truncate(
        KvOpts::STRUCT_LIKE.bits |
        KvOpts::STATEMENT_LIKE.bits |
        KvOpts::MATCH_LIKE.bits |
        KvOpts::TUPLE_ARRAY.bits),
    global: GlobalOpts::from_bits_truncate(
        GlobalOpts::STRING_IDENTS.bits |
        GlobalOpts::BARE_OPTION.bits),
};

/// A strict Rust-like configuration.
/// ```text
/// Input {
///     text: "Hello, World!",
///     hashmap: [
///         ([1], 1),
///         ([1, 2], 2),
///         ([1, 2, 3], 3),
///     ],
///     oneof: MyEnum::MyVariant {
///         data: ["a", "b", "c"],
///     },
/// }
/// ```
pub const RUSTY_STRICT: Config = Config {
    structs: KvOpts::STRUCT_LIKE,
    maps: KvOpts::TUPLE_ARRAY,
    global: GlobalOpts::empty(),
};

/// A Rust attribute-like configuration.
/// ```text
/// text("Hello, World!"),
/// hashmap(
///     [1] => 1,
///     [1, 2] => 2,
///     [1, 2, 3] => 3,
/// ),
/// oneof(MyVariant(data = ["a", "b", "c"])),
/// ```
pub const RUSTY_META: Config = Config {
    structs: KvOpts::from_bits_truncate(
        KvOpts::ATTRIBUTE_LIKE.bits |
        KvOpts::STATEMENT_LIKE.bits |
        KvOpts::BARE_UNIT.bits),
    maps: KvOpts::from_bits_truncate(
        KvOpts::ATTRIBUTE_LIKE.bits |
        KvOpts::STATEMENT_LIKE.bits |
        KvOpts::MATCH_LIKE.bits |
        KvOpts::BARE_UNIT.bits),
    global: GlobalOpts::from_bits_truncate(
        GlobalOpts::BARE_STRUCTS.bits |
        GlobalOpts::BARE_ENUMS.bits |
        GlobalOpts::BARE_OPTION.bits |
        GlobalOpts::STRING_IDENTS.bits |
        GlobalOpts::UNGROUPED_ROOT.bits),
};

/// A configuration to parse any combination of supported syntaxes.
/// ```text
/// {
///     text = "Hello, World!",
///     hashmap(
///         [1] => 1,
///         [1, 2] => 2,
///         [1, 2, 3] => 3,
///     ),
///     oneof: "MyVariant" {
///         data: ["a", "b", "c"],
///     },
/// }
/// ```
pub const ANYTHING_GOES: Config = Config {
    structs: KvOpts::all(),
    maps: KvOpts::all(),
    global: GlobalOpts::all(),
};
