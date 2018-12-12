#[macro_use]
extern crate serde_derive;

extern crate serde;
extern crate serde_json;
extern crate libc;

use std::ffi::CString;
use std::ffi::CStr;
use std::os::raw::c_char;
use std::fs::File;
use std::path::Path;
use std::ptr;
use std::mem;
use libc::{c_long, c_double};

/* Rust representations */
#[derive(Serialize, Deserialize, Debug)]
pub struct Command {
	platform:	CString,
	name:		CString,
    cmd:        CString,
    arg_fmt:    CString,
    args:       Vec<Argument>,
    help:       CString,
}

#[derive(Serialize, Deserialize, Debug)]
enum FieldType {
    FieldString(CString),
    // TODO Commands which display as yes/no that hide some actual command?
    // FieldBool(Bool),
    FieldInt(i64),
    FieldFloat(f64),
}

#[derive(Serialize, Deserialize, Debug)]
pub struct Argument {
    name:           CString,
    arg:            FieldType,
}

/* FFI respresentations */
#[repr(C)]
pub struct CommandArray {
    bytes:  *mut CommandRaw,
    len:    usize,
}

#[repr(C)]
pub struct CommandRaw {
    platform:   *mut c_char,
    name:       *mut c_char,
    cmd:        *mut c_char,
    arg_fmt:    *mut c_char,
    args:       *mut ArgumentRaw,
    n_args:     usize,
    help:       *mut c_char,
}

impl CommandRaw {
    /*
     * We need to convert several fields into FFI-suitable types and we
     * don't want to 'partially forget' the Command structs, so we
     * clone the various fields and forget them. The vec is then shrunk
     * and forgotten so it can be passed as an array C will recognise.
     */
    fn new(command: &Command) -> CommandRaw {
        CommandRaw {
            platform: command.platform.clone().into_raw(),
            name: command.name.clone().into_raw(),
            cmd: command.cmd.clone().into_raw(),
            arg_fmt: command.arg_fmt.clone().into_raw(),
            args: CommandRaw::args_into_raw(&command.args),
            n_args: command.args.len(),
            help: command.help.clone().into_raw()
        }
    }

    fn args_into_raw(args: &Vec<Argument>) -> *mut ArgumentRaw {

        let mut args_raw  = Vec::<ArgumentRaw>::new();

        for arg in args {
            let raw_arg = ArgumentRaw::new(&arg);
            args_raw.push(raw_arg);
        }

        args_raw.shrink_to_fit();
        let ptr = args_raw.as_mut_ptr();
        mem::forget(args_raw);

        return ptr;
    }
}

#[repr(C)]
/*
 * Rust enums are not C enums and Rust doesn't have a 'union' concept like in C
 * so we just declare all the fields here and set them based on arg_type.
 */
pub struct ArgumentRaw {
    name:       *mut c_char,
    arg_type:   FieldTypeRaw,
    arg_i64:    c_long,
    arg_f64:    c_double,
    arg_str:    *mut c_char,
}

impl ArgumentRaw {
    fn new(arg: &Argument) -> ArgumentRaw {
        let raw_arg = match arg.arg {
            FieldType::FieldString(ref s) => {
                ArgumentRaw {
                    name: arg.name.clone().into_raw(),
                    arg_type: FieldTypeRaw::RawString,
                    arg_i64: 0,
                    arg_f64: 0.0,
                    arg_str: s.clone().into_raw(),
                }
            },
            FieldType::FieldInt(i) => {
                ArgumentRaw {
                    name: arg.name.clone().into_raw(),
                    arg_type: FieldTypeRaw::RawInt,
                    arg_i64: i,
                    arg_f64: 0.0,
                    arg_str: ptr::null_mut(),
                }
            },
            FieldType::FieldFloat(f) => {
                ArgumentRaw {
                    name: arg.name.clone().into_raw(),
                    arg_type: FieldTypeRaw::RawFloat,
                    arg_i64: 0,
                    arg_f64: f,
                    arg_str: ptr::null_mut(),
                }
            },
        };
        return raw_arg;
    }
}

#[repr(C)]
#[derive(PartialEq)]
enum FieldTypeRaw {
    RawString = 0,
    RawInt,
    RawFloat,
}

/*
 * Take a CommandArray pointer from C and reclaim the CommandRaw
 * structs and their members/
 * We can't avoid the unsafe {} invocations since we're reading arbitrary
 * memory, but it's contained to this function.
 */
#[no_mangle]
pub extern fn free_command_array(commands: *mut CommandArray) {

    let ptr = unsafe { Box::from_raw(commands) };
    let raw_vec = unsafe { Vec::from_raw_parts(ptr.bytes,
                                               ptr.len,
                                               ptr.len) };
    for raw_command in raw_vec.iter() {
        unsafe {
            let _platform = CString::from_raw(raw_command.platform);
            let _name = CString::from_raw(raw_command.name);
            let _cmd = CString::from_raw(raw_command.cmd);
            let _arg_fmt = CString::from_raw(raw_command.arg_fmt);
            /* n_args  */
            let arg_vec = Vec::from_raw_parts(raw_command.args,
                                              raw_command.n_args,
                                              raw_command.n_args);
            for raw_arg in arg_vec.iter() {
                let _name = CString::from_raw(raw_arg.name);
                if raw_arg.arg_type == FieldTypeRaw::RawString {
                    let _arg_str = CString::from_raw(raw_arg.arg_str);
                }
            }
            let _help = CString::from_raw(raw_command.help);
        }
    }

    /* Frees once it goes out of scope */
    println!("Rust reclaimed {} command structs", raw_vec.len());
}

#[no_mangle]
pub extern fn parse_json(filename: *const c_char) -> *mut CommandArray {

    let file: String;
    unsafe {
        /* Can't trust the promise of a string from C */
        file = CStr::from_ptr(filename).to_string_lossy().into_owned();
    }

    let json_path = Path::new(&file);
    let json_file = match File::open(json_path) {
        Err(e) => {
            println!("Could not open file: {}", e);
            return ptr::null_mut();
        }
        Ok(f) => f,
    };

    let commands: Vec<Command> = match serde_json::from_reader(json_file) {
        Err(e) => {
            println!("Could not parse JSON: {}", e);
            return ptr::null_mut();
        }
        Ok(c) =>  c,
    };

    /* Convert commands into a form more easily passed to C */
    let mut ffi_vec = Vec::<CommandRaw>::new();
    for command in commands.iter() {
        let raw_command = CommandRaw::new(&command);
        ffi_vec.push(raw_command);
    }

    let len = ffi_vec.len();
    ffi_vec.shrink_to_fit();
    let ffi_ptr = ffi_vec.as_mut_ptr();
    mem::forget(ffi_vec);

    let wrapper = Box::new(CommandArray { bytes: ffi_ptr, len: len });
    return Box::into_raw(wrapper);
}
