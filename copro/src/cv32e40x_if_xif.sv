/*
 Copyright 2021 TU Wien

 Ten plik i jego pochodne są licencjonowane na podstawie licencji
 Solderpad License, Wersja 2.0 ("Licencja");
 Użycie tego pliku oznacza zgodę na warunki licencji 
 i pełną zgodność z Licencją.
 Kopię licencji można uzyskać pod adresem

 https://solderpad.org/licenses/SHL-2.0/

 O ile nie wymaga tego obowiązujące prawo lub nie zostało to uzgodnione na piśmie, oprogramowanie
 i implementacje sprzętowe rozprowadzane w ramach tej licencji są
 rozprowadzane na zasadzie "TAK JAK JEST", BEZ JAKICHKOLWIEK GWARANCJI LUB WARUNKÓW, WYRAŹNYCH LUB DOROZUMIANYCH.
 Zobacz Licencję w celu uzyskania szczegółowych informacji o uprawnieniach i 
 ograniczeniach w ramach Licencji.
 */

////////////////////////////////////////////////////////////////////////////////
// Inżynier:       Michael Platzer - michael.platzer@tuwien.ac.at             //
//                                                                            //
// Nazwa projektu: CORE-V XIF eXtension Interface                             //
// Nazwa modułu:   RI5CY                                                      //
// Język:          SystemVerilog                                              //
//                                                                            //
// Opis:           Definicja interfejsu rozszerzeń CORE-V XIF.                //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

interface cv32e40x_if_xif
#(
  parameter int unsigned X_NUM_RS        =  2,  // Liczba portów odczytu pliku rejestrów, które mogą być użyte przez interfejs rozszerzeń
  parameter int unsigned X_ID_WIDTH      =  4,  // Szerokość pola ID.
  parameter int unsigned X_MEM_WIDTH     =  32, // Szerokość dostępu do pamięci dla operacji odczytu/zapisu przez interfejs rozszerzeń
  parameter int unsigned X_RFR_WIDTH     =  32, // Szerokość dostępu odczytu pliku rejestrów dla interfejsu rozszerzeń
  parameter int unsigned X_RFW_WIDTH     =  32, // Szerokość dostępu zapisu pliku rejestrów dla interfejsu rozszerzeń
  parameter logic [31:0] X_MISA          =  '0, // Rozszerzenia MISA zaimplementowane w interfejsie rozszerzeń
  parameter logic [ 1:0] X_ECS_XS        =  '0  // Domyślna wartość dla mstatus.XS
);

  localparam int XLEN = 32;
  localparam int FLEN = 32;

  typedef struct packed {
    logic [          15:0] instr; // Zlecona skompresowana instrukcja
    logic [           1:0] mode;  // Poziom uprawnień
    logic [X_ID_WIDTH-1:0] id;    // Numer identyfikacyjny zleconej skompresowanej instrukcji
  } x_compressed_req_t;

  typedef struct packed {
    logic [31:0] instr;   // Nieskompresowana instrukcja
    logic        accept;  // Czy zlecona skompresowana instrukcja (id) została zaakceptowana przez koprocesor?
  } x_compressed_resp_t;

  typedef struct packed {
    logic [          31:0]                  instr;     // Zlecona instrukcja
    logic [           1:0]                  mode;      // Poziom uprawnień
    logic [X_ID_WIDTH-1:0]                  id;        // Identyfikacja zleconej instrukcji
    logic [X_NUM_RS  -1:0][X_RFR_WIDTH-1:0] rs;        // Operandy źródłowe pliku rejestrów dla zleconej instrukcji
    logic [X_NUM_RS  -1:0]                  rs_valid;  // Ważność operandu(ów) źródłowych pliku rejestrów
    logic [           5:0]                  ecs;       // Status Kontekstu Rozszerzenia (Extension Context Status) ({mstatus.xs, mstatus.fs, mstatus.vs})
    logic                                   ecs_valid; // Ważność Statusu Kontekstu Rozszerzenia
  } x_issue_req_t;

  typedef struct packed {
    logic       accept;     // Czy zlecona instrukcja (id) została zaakceptowana przez koprocesor?
    logic       writeback;  // Czy koprocesor wykona zapis zwrotny (writeback) w rdzeniu do rd?
    logic       dualwrite;  // Czy koprocesor wykona podwójny zapis zwrotny w rdzeniu do rd i rd+1?
    logic [2:0] dualread;   // Czy koprocesor będzie wymagał podwójnych odczytów z rs1\rs2\rs3 oraz rs1+1\rs2+1\rs3+1?
    logic       loadstore;  // Czy zlecona instrukcja jest instrukcją odczytu/zapisu pamięci (load/store)?
    logic       ecswrite ;  // Czy koprocesor zapisze Status Kontekstu Rozszerzenia w mstatus?
    logic       exc;        // Czy zlecona instrukcja może ewentualnie spowodować synchroniczny wyjątek w samym koprocesorze?
  } x_issue_resp_t;

  typedef struct packed {
    logic [X_ID_WIDTH-1:0] id;          // Identyfikacja zleconej instrukcji
    logic                  commit_kill; // Czy zlecona instrukcja powinna zostać anulowana (zabita)?
  } x_commit_t;

  typedef struct packed {
    logic [X_ID_WIDTH   -1:0] id;    // Identyfikacja zleconej instrukcji
    logic [             31:0] addr;  // Wirtualny adres transakcji pamięciowej
    logic [              1:0] mode;  // Poziom uprawnień
    logic                     we;    // Sygnał zezwolenia na zapis (write enable) dla transakcji pamięciowej
    logic [              2:0] size;  // Rozmiar transakcji pamięciowej
    logic [X_MEM_WIDTH/8-1:0] be;    // Zezwolenia na bajty (byte enables) dla transakcji pamięciowej
    logic [              1:0] attr;  // Atrybuty transakcji pamięciowej
    logic [X_MEM_WIDTH  -1:0] wdata; // Dane do zapisu dla transakcji pamięciowej typu store
    logic                     last;  // Czy to jest ostatnia transakcja pamięciowa dla zleconej instrukcji?
    logic                     spec;  // Czy transakcja pamięciowa jest spekulatywna?
  } x_mem_req_t;

  typedef struct packed {
    logic       exc;      // Czy żądanie pamięci spowodowało wyjątek synchroniczny?
    logic [5:0] exccode;  // Kod wyjątku
    logic       dbg;      // Czy żądanie pamięci spowodowało dopasowanie wyzwalacza debugowania (trigger match) z ``mcontrol.timing`` = 0?
  } x_mem_resp_t;

  typedef struct packed {
    logic [X_ID_WIDTH -1:0] id;     // Identyfikacja zleconej instrukcji
    logic [X_MEM_WIDTH-1:0] rdata;  // Odczytane dane z transakcji pamięciowej typu read
    logic                   err;    // Czy instrukcja spowodowała błąd magistrali (bus error)?
    logic                   dbg;    // Czy odczytane dane spowodowały dopasowanie wyzwalacza debugowania z ``mcontrol.timing`` = 0?
  } x_mem_result_t;

  typedef struct packed {
    logic [X_ID_WIDTH      -1:0] id;      // Identyfikacja zleconej instrukcji
    logic [X_RFW_WIDTH     -1:0] data;    // Wartość(-ci) danych do zapisu do pliku rejestrów
    logic [                 4:0] rd;      // Adres(-y) docelowe w pliku rejestrów
    logic [X_RFW_WIDTH/XLEN-1:0] we;      // Zezwolenie(-a) na zapis do pliku rejestrów
    logic [                 5:0] ecsdata; // Wartość danych do zapisu dla {mstatus.xs, mstatus.fs, mstatus.vs}
    logic [                 2:0] ecswe;   // Zezwolenia na zapis dla {mstatus.xs, mstatus.fs, mstatus.vs}
    logic                        exc;     // Czy instrukcja spowodowała wyjątek synchroniczny?
    logic [                 5:0] exccode; // Kod wyjątku
    logic                        err;     // Czy instrukcja spowodowała błąd magistrali (bus error)?
    logic                        dbg;     // Czy instrukcja spowodowała dopasowanie wyzwalacza debugowania z ``mcontrol.timing`` = 0?
  } x_result_t;

  // Interfejs skompresowany
  logic               compressed_valid;
  logic               compressed_ready;
  x_compressed_req_t  compressed_req;
  x_compressed_resp_t compressed_resp;

  // Interfejs wydawania (issue)
  logic               issue_valid;
  logic               issue_ready;
  x_issue_req_t       issue_req;
  x_issue_resp_t      issue_resp;

  // Interfejs zatwierdzania (commit)
  logic               commit_valid;
  x_commit_t          commit;

  // Interfejs pamięci (żądanie/odpowiedź)
  logic               mem_valid;
  logic               mem_ready;
  x_mem_req_t         mem_req;
  x_mem_resp_t        mem_resp;

  // Interfejs wyników pamięci
  logic               mem_result_valid;
  x_mem_result_t      mem_result;

  // Interfejs wyników
  logic               result_valid;
  logic               result_ready;
  x_result_t          result;

  // Kierunki portów dla głównego procesora (host CPU)
  modport cpu_compressed (
    output compressed_valid,
    input  compressed_ready,
    output compressed_req,
    input  compressed_resp
  );
  modport cpu_issue (
    output issue_valid,
    input  issue_ready,
    output issue_req,
    input  issue_resp
  );
  modport cpu_commit (
    output commit_valid,
    output commit
  );
  modport cpu_mem (
    input  mem_valid,
    output mem_ready,
    input  mem_req,
    output mem_resp
  );
  modport cpu_mem_result (
    output mem_result_valid,
    output mem_result
  );
  modport cpu_result (
    input  result_valid,
    output result_ready,
    input  result
  );

  // Kierunki portów dla rozszerzenia (koprocesora)
  modport coproc_compressed (
    input  compressed_valid,
    output compressed_ready,
    input  compressed_req,
    output compressed_resp
  );
  modport coproc_issue (
    input  issue_valid,
    output issue_ready,
    input  issue_req,
    output issue_resp
  );
  modport coproc_commit (
    input  commit_valid,
    input  commit
  );
  modport coproc_mem (
    output mem_valid,
    input  mem_ready,
    output mem_req,
    input  mem_resp
  );
  modport coproc_mem_result (
    input  mem_result_valid,
    input  mem_result
  );
  modport coproc_result (
    output result_valid,
    input  result_ready,
    output result
  );

  // Kierunki portów dla monitora
  modport monitor_compressed (
    input  compressed_valid,
    input  compressed_ready,
    input  compressed_req,
    input  compressed_resp
  );
  modport monitor_issue (
    input  issue_valid,
    input  issue_ready,
    input  issue_req,
    input  issue_resp
  );
  modport monitor_commit (
    input  commit_valid,
    input  commit
  );
  modport monitor_mem (
    input  mem_valid,
    input  mem_ready,
    input  mem_req,
    input  mem_resp
  );
  modport monitor_mem_result (
    input  mem_result_valid,
    input  mem_result
  );
  modport monitor_result (
    input  result_valid,
    input  result_ready,
    input  result
  );

endinterface : cv32e40x_if_xif